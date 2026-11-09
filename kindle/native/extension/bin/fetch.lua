-- rm2manga HTTPS helper for Kindle.
-- KindleOS ships OpenSSL 1.0.2 (wget/curl cannot negotiate TLS with MangaDex),
-- so we borrow KOReader's bundled LuaSocket + LuaSec (modern TLS).
--
-- Usage (run with cwd=/mnt/us/koreader via fetch.sh):
--   fetch.lua get   <url> <outfile> [referer]
--   fetch.lua batch <listfile>        (each line: "<url>\t<outfile>")
require("setupkoenv")
local http = require("socket.http")
local ltn12 = require("ltn12")

http.TIMEOUT = 30
local UA = "rm2manga/1.0 (Kindle Oasis)"

local function fetch(url, out, referer)
    local part = out .. ".part"
    local f = io.open(part, "wb")
    if not f then return false end
    local headers = { ["User-Agent"] = UA }
    if referer and referer ~= "" then headers["Referer"] = referer end
    local ok, code = pcall(function()
        local _, c = http.request{
            url = url,
            sink = ltn12.sink.file(f), -- closes f when done
            headers = headers,
            redirect = true,
        }
        return c
    end)
    if ok and code == 200 then
        os.remove(out)
        os.rename(part, out)
        return true
    end
    pcall(function() f:close() end)
    os.remove(part)
    io.stderr:write("fetch failed (" .. tostring(code) .. "): " .. url .. "\n")
    return false
end

local mode = arg[1]
if mode == "get" then
    local ok = fetch(arg[2], arg[3], arg[4]) or fetch(arg[2], arg[3], arg[4])
    os.exit(ok and 0 or 1)
elseif mode == "batch" then
    local failures = 0
    for line in io.lines(arg[2]) do
        local url, out = line:match("^(.-)\t(.+)$")
        if url then
            if not (fetch(url, out) or fetch(url, out)) then
                failures = failures + 1
            end
        end
    end
    os.exit(failures == 0 and 0 or 1)
else
    io.stderr:write("usage: fetch.lua get <url> <out> [referer] | batch <list>\n")
    os.exit(2)
end
