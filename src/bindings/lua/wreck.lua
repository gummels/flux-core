--[[--------------------------------------------------------------------------
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
 ---------------------------------------------------------------------------]]
local posix = require 'flux.posix'

local wreck = {}
wreck.__index = wreck;

local lwj_options = {
--    ['ntasks'] =                "Set total number of tasks to execute",
    ['commit-on-task-exit'] =   "Call kvs_commit for each task exit",
    ['stdio-delay-commit'] =    "Don't call kvs_commit for each line of output",
    ['stdio-commit-on-open'] =  "Commit to kvs on stdio open in each task",
    ['stdio-commit-on-close'] = "Commit to kvs on stdio close in each task",
    ['stop-children-in-exec'] = "Start tasks in STOPPED state for debugger",
}

local default_opts = {
    ['help']    = { char = 'h'  },
    ['verbose'] = { char = 'v'  },
    ['ntasks']  = { char = 'n', arg = "N" },
    ['walltime'] = { char = "T", arg = "SECONDS" },
    ['options'] = { char = 'o', arg = "OPTIONS.." },
}

local function opt_table (w)
    local o = {}
    for x,t in pairs (default_opts) do
        o[x] = t.char
    end
    for _,v in pairs (w.extra_options) do
        o[v.name] = v.char
    end
    return o
end

local function short_opts (w)
    local s = ""
    for x,t in pairs (default_opts) do
        s = s .. t.char .. (t.arg and ":" or "")
    end
    for _,v in pairs (w.extra_options) do
        s = s .. v.char .. (v.arg and ":" or "")
    end
    return s
end

function wreck:say (...)
    io.stderr:write (self.prog..": "..string.format (...))
end

function wreck:verbose (...)
    if self.opts.v then
        self:say (...)
    end
end

function wreck:die (...)
    self:say (...)
    os.exit (1)
end

function wreck:usage()
    io.stderr:write ("Usage: "..self.prog.." OPTIONS.. COMMANDS..\n")
    io.stderr:write ([[
  -h, --help                 Display this message
  -v, --verbose              Be verbose
  -n, --ntasks=N             Request to run a total of N tasks
  -o, --options=OPTION,...   Set other options (See OTHER OPTIONS below)
  -T, --walltime=N[SUFFIX]   Set max job walltime to N seconds. Optional
                             suffix may be 's' for seconds (default), 'm'
                             for minutes, 'h' for hours or 'd' for days.
                             N may be an arbitrary floating point number,
                             but will be rounded up to nearest second.
]])
    for _,v in pairs (self.extra_options) do
        local optstr = v.name .. (v.arg and "="..v.arg or "")
        io.stderr:write (
            string.format ("  -%s, --%-20s %s\n", v.char, optstr, v.usage))
    end
    io.stderr:write ("\nOTHER OPTIONS:\n")
    for o,d in pairs (lwj_options) do
        io.stderr:write (string.format ("  %-26s %s\n", o, d))
    end
end

local function get_filtered_env ()
    local env = posix.getenv()
    env.HOSTNAME = nil
    env.ENVIRONMENT = nil
    for k,v in pairs (env) do
        if k:match ("SLURM_") then env[k] = nil end
        if k:match ("FLUX_API") then env[k] = nil end
        if k:match ("FLUX_URI") then env[k] = nil end
    end
    -- XXX: MVAPICH2 at least requires MPIRUN_RSH_LAUNCH to be set
    --  in the environment or PMI doesn't work (for unknown reason)
    env.MPIRUN_RSH_LAUNCH = 1
    return (env)
end

function wreck:add_options (opts)
    for _,v in pairs (opts) do
        if not type (v) == "table" then return nil, "Invalid parameter" end

        local char = v.char
        if default_opts [v.name] then
            return nil, "Can't override option '"..k.."'"
        end
        table.insert (self.extra_options, v)
    end
    return true
end

function wreck:getopt (opt)
    if not self.opts then return nil  end
    return self.opts [opt]
end

local function parse_walltime (s)
    local m = { s = 1, m = 60, h = 3600, d = 56400 }
    local n, suffix = s:match ("^([0-9.]+)([HhMmDdSs]?)$")
    if not tonumber (n) then
        return nil, "Invalid duration '"..s.."'"
    end
    if suffix and m [suffix]  then
        n = (n * m [suffix])
    end
    return math.ceil (n)
end

function wreck:parse_cmdline (arg)
    local getopt = require 'flux.alt_getopt' .get_opts
    local s = short_opts (self)
    local v = opt_table (self)
    self.opts, self.optind = getopt (arg, s, v)

    if self.opts.h then
        self:usage()
        os.exit (0)
    end

    if self.opts.o then
        for opt in self.opts.o:gmatch ("[^,]+") do
            if not lwj_options [opt] then
                return nil, string.format ("Unknown LWJ option '%s'\n", opt)
            end
        end
    end

    if self.opts.T then
        self.walltime, err = parse_walltime (self.opts.T)
        if not self.walltime then
            self:die ("Error: %s", err)
        end
    end

    if self.optind > #arg then
        self:say ("Error: remote command required\n")
        self:usage()
        os.exit (1)
    end

    self.nnodes = tonumber (self.opts.N) or 1
    self.ntasks = tonumber (self.opts.n) or 1

    self.cmdline = {}
    for i = self.optind, #arg do
        table.insert (self.cmdline, arg[i])
    end

    return true
end

function wreck:jobreq ()
    if not self.opts then return nil, "Error: cmdline not parsed" end
    local nnodes = tonumber (self.opts.N)
    local ntasks = tonumber (self.opts.n)
    local jobreq = {
        nnodes =  tonumber (self.opts.N) or 1,
        ntasks =  tonumber (self.opts.n) or 1,
        cmdline = self.cmdline,
        environ = get_filtered_env (),
        cwd =     posix.getcwd (),
        walltime =self.walltime or 0
    }
    if self.opts.o then
        for opt in self.opts.o:gmatch ('[^,]+') do
            jobreq['options.'..opt] = 1
        end
    end
    return jobreq
end

function wreck.ioattach (arg)
    local ioplex = require 'wreck.io'
    local f = arg.flux
    if not arg.ntasks then
        local lwj, err = f:kvsdir ("lwj."..arg.jobid)
        if not lwj then
            return nil, "Error: "..err
        end
        arg.ntasks = lwj.ntasks
    end
    local taskio, err = ioplex.create (arg)
    if not taskio then return nil, err end
    for i = 0, arg.ntasks - 1 do
        for _,stream in pairs {"stdout", "stderr"} do
            local rc, err =  taskio:redirect (i, stream, stream)
        end
    end
    taskio:start (arg.flux)

    return taskio
end

local function exit_message (t)
    local s = "exited with "
    s = s .. (t.exit_code and "exit code" or "signal") .. " %d"
    return s:format (t.exit_code or t.exit_sig)
end

local function task_status (lwj, taskid)
    if not tonumber (taskid) then return nil end
    local t = lwj[taskid]
    if not t.exit_status then
        return 0, (t.procdesc and "starting" or "running")
    end
    local x = t.exit_code or (t.exit_sig + 128)
    return x, exit_message (t)
end

--- Return highest exit code from all tasks and task exit message list.
-- Summarize job exit status for arg.jobid by returning max task exit code,
-- along with a list of task exit messages to be optionally emitted to stdout.
-- @param arg.jobid job identifier
-- @param arg.flux  (optional) flux handle
-- @return exit_cde, msg_list
function wreck.status (arg)
    local hostlist = require 'flux.hostlist'
    local f = arg.flux
    local jobid = arg.jobid
    if not jobid then return nil, "required arg jobid" end

    if not f then f = require 'flux'.new() end
    local lwj = f:kvsdir ("lwj."..jobid)
    local max = 0
    local msgs = {}
    for taskid in lwj:keys () do
        local x, s = task_status (lwj, taskid)
        if x then
            if x > max then max = x end
            if not msgs[s] then
                msgs[s] = hostlist.new (taskid)
            else
                msgs[s]:concat (taskid)
            end
        end
    end
    return max, msgs
end

function wreck.new (prog)
    local w = setmetatable ({extra_options = {}}, wreck) 
    w.prog = prog
    return w
end

return wreck

-- vi: ts=4 sw=4 expandtab
