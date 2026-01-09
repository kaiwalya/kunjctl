-- home_automation
-- xmake as a task runner for ESP-IDF projects

set_project("home_automation")
set_version("0.1.0")

----------------------------------------------------------------------
-- Auto-discover setups from setups/<subproject>/<chip>/<name>.conf
-- Build directory mirrors: build/<subproject>/<chip>/<name>/
----------------------------------------------------------------------

local subprojects = {"thread-end-device", "thread-router", "thread-rcp"}
local chips = {"esp32h2", "esp32s3", "esp32c6"}

local setups = {}
for _, subproject in ipairs(subprojects) do
    for _, chip in ipairs(chips) do
        local setup_dir = path.join("setups", subproject, chip)
        local configs = os.files(path.join(setup_dir, "*.conf"))
        for _, conf in ipairs(configs) do
            local name = path.basename(conf):gsub("%.conf$", "")
            local setup_name = subproject .. "-" .. chip .. "-" .. name
            setups[setup_name] = { subproject = subproject, chip = chip, name = name, conf = conf }
        end
    end
end

----------------------------------------------------------------------
-- Helper: validate setup and raise helpful error
----------------------------------------------------------------------
local function require_setup(setup, raise)
    if not setup then
        local available = {}
        for name, _ in pairs(setups) do
            table.insert(available, name)
        end
        table.sort(available)
        raise("Setup required. Available setups:\n  " .. table.concat(available, "\n  "))
    end
    if not setups[setup] then
        -- Try to guess what path they meant
        local parts = {}
        for part in setup:gmatch("[^-]+") do
            table.insert(parts, part)
        end
        local guessed_path = "setups/<subproject>/<chip>/<name>.conf"
        if #parts >= 3 then
            guessed_path = string.format("setups/%s/%s/%s.conf",
                parts[1],
                parts[2],
                table.concat(parts, "-", 3))
        end
        raise("Setup '%s' not found.\nExpected config at: %s", setup, guessed_path)
    end
    return setups[setup]
end

----------------------------------------------------------------------
-- Helper: get idf.py command prefix
----------------------------------------------------------------------
local function idf_py()
    return string.format('%s/bin/python3 "%s/tools/idf.py"',
        os.getenv("IDF_PYTHON_ENV_PATH"),
        os.getenv("IDF_PATH"))
end

----------------------------------------------------------------------
-- Build targets: node-devkitm-h2, hub-devkitm-s3, etc.
----------------------------------------------------------------------

for setup_name, setup in pairs(setups) do
    target(setup_name)
        set_kind("phony")
        on_build(function (target)
            local s = setups[target:name()]
            -- Build path mirrors setups: build/<subproject>/<chip>/<name>/
            local build_dir = path.join("build", s.subproject, s.chip, s.name)

            local sdkconfig_defaults = "sdkconfig.defaults;" .. s.conf

            -- Auto set-target if CMakeCache is missing
            local cmake_cache = path.join(build_dir, "CMakeCache.txt")
            if not os.isfile(cmake_cache) then
                os.exec('%s -B %s -D SDKCONFIG_DEFAULTS="%s" set-target %s',
                    idf_py(), build_dir, sdkconfig_defaults, s.chip)
            end
            os.exec('%s -B %s build', idf_py(), build_dir)
        end)
        on_clean(function (target)
            local s = setups[target:name()]
            local build_dir = path.join("build", s.subproject, s.chip, s.name)
            os.exec('%s -B %s fullclean', idf_py(), build_dir)
        end)
end

----------------------------------------------------------------------
-- Tasks
----------------------------------------------------------------------

task("flash")
    set_category("plugin")
    set_menu {
        usage = "xmake flash <setup> [-p <port>]",
        description = "Flash firmware to device",
        options = {
            {nil, "setup", "v", nil, "Setup name (e.g., node-esp32h2-devkitm)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local s = require_setup(option.get("setup"), raise)
        local port = option.get("port")
        local build_dir = path.join("build", s.subproject, s.chip, s.name)
        local port_arg = port and ("-p " .. port) or ""
        os.exec('%s -B %s %s flash', idf_py(), build_dir, port_arg)
    end)

task("monitor")
    set_category("plugin")
    set_menu {
        usage = "xmake monitor <setup> [-p <port>]",
        description = "Monitor serial output",
        options = {
            {nil, "setup", "v", nil, "Setup name (e.g., node-esp32h2-devkitm)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local s = require_setup(option.get("setup"), raise)
        local port = option.get("port")
        local build_dir = path.join("build", s.subproject, s.chip, s.name)
        local port_arg = port and ("-p " .. port) or ""
        os.exec('%s -B %s %s monitor', idf_py(), build_dir, port_arg)
    end)

task("fm")
    set_category("plugin")
    set_menu {
        usage = "xmake fm <setup> [-p <port>]",
        description = "Flash and monitor",
        options = {
            {nil, "setup", "v", nil, "Setup name (e.g., node-esp32h2-devkitm)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local s = require_setup(option.get("setup"), raise)
        local port = option.get("port")
        local build_dir = path.join("build", s.subproject, s.chip, s.name)
        local port_arg = port and ("-p " .. port) or ""
        os.exec('%s -B %s %s flash monitor', idf_py(), build_dir, port_arg)
    end)

task("menuconfig")
    set_category("plugin")
    set_menu {
        usage = "xmake menuconfig <setup>",
        description = "Open IDF menuconfig",
        options = {
            {nil, "setup", "v", nil, "Setup name (e.g., node-esp32h2-devkitm)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local s = require_setup(option.get("setup"), raise)
        local build_dir = path.join("build", s.subproject, s.chip, s.name)
        os.exec('%s -B %s menuconfig', idf_py(), build_dir)
    end)

task("size")
    set_category("plugin")
    set_menu {
        usage = "xmake size <setup>",
        description = "Show firmware size info",
        options = {
            {nil, "setup", "v", nil, "Setup name (e.g., node-esp32h2-devkitm)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local s = require_setup(option.get("setup"), raise)
        local build_dir = path.join("build", s.subproject, s.chip, s.name)
        os.exec('%s -B %s size', idf_py(), build_dir)
    end)

task("codegen")
    set_category("plugin")
    set_menu {
        usage = "xmake codegen",
        description = "Regenerate protobuf files with nanopb",
        options = {}
    }
    on_run(function ()
        local proto_dirs = {
            "components/thread_comms/proto",
        }
        for _, proto_dir in ipairs(proto_dirs) do
            local proto_files = os.files(path.join(proto_dir, "*.proto"))
            if #proto_files > 0 then
                print("Processing: " .. proto_dir)
                local old_dir = os.cd(proto_dir)
                for _, proto_file in ipairs(proto_files) do
                    local filename = path.filename(proto_file)
                    print("  Generating: " .. filename)
                    os.exec("nanopb_generator %s", filename)
                end
                os.cd(old_dir)
            end
        end
        print("Codegen complete")
    end)
