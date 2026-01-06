-- home_automation
-- xmake as a task runner for ESP-IDF projects

set_project("home_automation")
set_version("0.1.0")

----------------------------------------------------------------------
-- Build targets (phony)
-- Creates targets like: main-esp32h2, main-esp32s3
----------------------------------------------------------------------

local chips = {"esp32h2", "esp32s3"}
local subprojects = {"main"}  -- Add more subprojects here later

for _, subproject in ipairs(subprojects) do
    for _, chip in ipairs(chips) do
        local target_name = subproject .. "-" .. chip
        target(target_name)
            set_kind("phony")
            on_build(function (target)
                local parts = target:name():split("-")
                local proj = parts[1]
                local c = parts[2]
                local build_dir = path.join("build", proj, c)
                os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s build',
                    os.getenv("IDF_PYTHON_ENV_PATH"),
                    os.getenv("IDF_PATH"),
                    build_dir)
            end)
            on_clean(function (target)
                local parts = target:name():split("-")
                local proj = parts[1]
                local c = parts[2]
                local build_dir = path.join("build", proj, c)
                os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s fullclean',
                    os.getenv("IDF_PYTHON_ENV_PATH"),
                    os.getenv("IDF_PATH"),
                    build_dir)
            end)
    end
end

-- Build all targets
target("all")
    set_kind("phony")
    set_default(true)
    on_build(function ()
        for _, subproject in ipairs({"main"}) do
            for _, chip in ipairs({"esp32h2", "esp32s3"}) do
                local build_dir = path.join("build", subproject, chip)
                os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s build',
                    os.getenv("IDF_PYTHON_ENV_PATH"),
                    os.getenv("IDF_PATH"),
                    build_dir)
            end
        end
    end)

----------------------------------------------------------------------
-- Tasks
----------------------------------------------------------------------

task("set-target")
    set_category("plugin")
    set_menu {
        usage = "xmake set-target [-s <subproject>] [-c <chip>]",
        description = "Configure IDF target (run once per chip)",
        options = {
            {'s', "subproject", "kv", nil, "Subproject (default: all)"},
            {'c', "chip", "kv", nil, "Chip (esp32h2, esp32s3, or 'all')"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject") or "all"
        local chip = option.get("chip") or "all"

        local subprojects = {"main"}
        local chips = {"esp32h2", "esp32s3"}

        local proj_list = subproject == "all" and subprojects or {subproject}
        local chip_list = chip == "all" and chips or {chip}

        for _, p in ipairs(proj_list) do
            for _, c in ipairs(chip_list) do
                local build_dir = path.join("build", p, c)
                os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s set-target %s',
                    os.getenv("IDF_PYTHON_ENV_PATH"),
                    os.getenv("IDF_PATH"),
                    build_dir, c)
            end
        end
    end)

task("flash")
    set_category("plugin")
    set_menu {
        usage = "xmake flash -s <subproject> -c <chip> [-p <port>]",
        description = "Flash firmware to device",
        options = {
            {'s', "subproject", "kv", nil, "Subproject name"},
            {'c', "chip", "kv", nil, "Target chip (esp32h2, esp32s3)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject")
        local chip = option.get("chip")
        local port = option.get("port")
        if not subproject or not chip then
            raise("required: xmake flash -s main -c esp32h2")
        end
        local build_dir = path.join("build", subproject, chip)
        if port then
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s -p %s flash',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir, port)
        else
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s flash',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir)
        end
    end)

task("monitor")
    set_category("plugin")
    set_menu {
        usage = "xmake monitor -s <subproject> -c <chip> [-p <port>]",
        description = "Monitor serial output",
        options = {
            {'s', "subproject", "kv", nil, "Subproject name"},
            {'c', "chip", "kv", nil, "Target chip (esp32h2, esp32s3)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject")
        local chip = option.get("chip")
        local port = option.get("port")
        if not subproject or not chip then
            raise("required: xmake monitor -s main -c esp32h2")
        end
        local build_dir = path.join("build", subproject, chip)
        if port then
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s -p %s monitor',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir, port)
        else
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s monitor',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir)
        end
    end)

task("fm")
    set_category("plugin")
    set_menu {
        usage = "xmake fm -s <subproject> -c <chip> [-p <port>]",
        description = "Flash and monitor",
        options = {
            {'s', "subproject", "kv", nil, "Subproject name"},
            {'c', "chip", "kv", nil, "Target chip (esp32h2, esp32s3)"},
            {'p', "port", "kv", nil, "Serial port (optional)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject")
        local chip = option.get("chip")
        local port = option.get("port")
        if not subproject or not chip then
            raise("required: xmake fm -s main -c esp32h2")
        end
        local build_dir = path.join("build", subproject, chip)
        if port then
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s -p %s flash monitor',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir, port)
        else
            os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s flash monitor',
                os.getenv("IDF_PYTHON_ENV_PATH"),
                os.getenv("IDF_PATH"),
                build_dir)
        end
    end)

task("menuconfig")
    set_category("plugin")
    set_menu {
        usage = "xmake menuconfig -s <subproject> -c <chip>",
        description = "Open IDF menuconfig",
        options = {
            {'s', "subproject", "kv", nil, "Subproject name"},
            {'c', "chip", "kv", nil, "Target chip (esp32h2, esp32s3)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject")
        local chip = option.get("chip")
        if not subproject or not chip then
            raise("required: xmake menuconfig -s main -c esp32h2")
        end
        local build_dir = path.join("build", subproject, chip)
        os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s menuconfig',
            os.getenv("IDF_PYTHON_ENV_PATH"),
            os.getenv("IDF_PATH"),
            build_dir)
    end)

task("size")
    set_category("plugin")
    set_menu {
        usage = "xmake size -s <subproject> -c <chip>",
        description = "Show firmware size info",
        options = {
            {'s', "subproject", "kv", nil, "Subproject name"},
            {'c', "chip", "kv", nil, "Target chip (esp32h2, esp32s3)"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local subproject = option.get("subproject")
        local chip = option.get("chip")
        if not subproject or not chip then
            raise("required: xmake size -s main -c esp32h2")
        end
        local build_dir = path.join("build", subproject, chip)
        os.exec('%s/bin/python3 "%s/tools/idf.py" -B %s size',
            os.getenv("IDF_PYTHON_ENV_PATH"),
            os.getenv("IDF_PATH"),
            build_dir)
    end)
