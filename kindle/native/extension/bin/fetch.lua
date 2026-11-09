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
local BROWSER_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
local MANGADEX_UA = "rm2manga/1.0 (https://github.com/tle2721/remarkable2.MangaReader)"

local function fetch(url, out, referer)
    if not url or url == "" then return false end
    if url:sub(1, 2) == "//" then url = "https:" .. url end

    local part = out .. ".part"
    local f = io.open(part, "wb")
    if not f then return false end
    local ua = url:find("mangadex%.org") and MANGADEX_UA or BROWSER_UA
    local headers = { ["User-Agent"] = ua }
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
