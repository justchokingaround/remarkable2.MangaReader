-- rm2manga HTTPS helper for Kindle.
-- KindleOS ships OpenSSL 1.0.2 (wget/curl cannot negotiate TLS with MangaDex),
-- so we borrow KOReader's bundled LuaSocket + LuaSec (modern TLS).
--
-- Usage (run with cwd=/mnt/us/koreader via fetch.sh):
--   fetch.lua get   <url> <outfile> [referer] [errfile]
--   fetch.lua batch <listfile>        (each line: "<url>\t<outfile>[\t<referer>]")
require("setupkoenv")
local http = require("socket.http")
local ltn12 = require("ltn12")

http.TIMEOUT = 30
local BROWSER_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
local MANGADEX_UA = "rm2manga/1.0 (https://github.com/tle2721/remarkable2.MangaReader)"

local function resolveUrl(base, ref)
    if not ref or ref == "" then return base end
    if ref:find("^https?://") then
        return ref
    elseif ref:sub(1, 2) == "//" then
        local proto = base:match("^(https?:)") or "https:"
        return proto .. ref
    elseif ref:sub(1, 1) == "/" then
        local origin = base:match("^(https?://[^/]+)")
        return (origin or "") .. ref
    else
        local dir = base:match("^(https?://.*/)")
        return (dir or (base .. "/")) .. ref
    end
end

local function getReferer(url)
    if not url then return nil end
    if url:find("mfcdn%.net") or url:find("fanfox%.net") or url:find("mangafox") or url:find("zjcdn") then
        return "https://fanfox.net/"
    elseif url:find("mangahere") then
        return "https://www.mangahere.cc/"
    elseif url:find("mangakatana%.com") then
        return "https://mangakatana.com/"
    elseif url:find("mangadex%.org") then
        return "https://mangadex.org/"
    end
    local origin = url:match("^(https?://[^/]+)")
    if origin then return origin .. "/" end
    return nil
end

local function getUA(url)
    if url and url:find("mangadex%.org") then
        return MANGADEX_UA
    end
    return BROWSER_UA
end

local function fetch(url, out, referer)
    if not url or url == "" then return false, "empty url" end
    if url:sub(1, 2) == "//" then url = "https:" .. url end

    local currentUrl = url
    local maxRedirects = 5

    for hop = 1, maxRedirects do
        local part = out .. ".part"
        local f = io.open(part, "wb")
        if not f then return false, "cannot open part file: " .. tostring(part) end

        local activeRef = (referer and referer ~= "") and referer or getReferer(currentUrl)
        local reqHeaders = { ["User-Agent"] = getUA(currentUrl) }
        if activeRef and activeRef ~= "" then
            reqHeaders["Referer"] = activeRef
        end

        local respHeaders
        local ok, code, status = pcall(function()
            local _, c, h, statusLine = http.request{
                url = currentUrl,
                sink = ltn12.sink.file(f), -- closes f when done
                headers = reqHeaders,
            }
            respHeaders = h
            return c, statusLine
        end)
        pcall(function() f:close() end)

        if ok and (code == 301 or code == 302 or code == 303 or code == 307 or code == 308) then
            os.remove(part)
            local location = respHeaders and (respHeaders["location"] or respHeaders["Location"])
            if location and location ~= "" then
                currentUrl = resolveUrl(currentUrl, location)
            else
                return false, "HTTP " .. tostring(code) .. " missing Location header"
            end
        elseif ok and code == 200 then
            os.remove(out)
            os.rename(part, out)
            return true, nil
        else
            os.remove(part)
            local reason
            if not ok then
                reason = "request error: " .. tostring(code)
            elseif type(code) == "number" then
                reason = "HTTP " .. tostring(code) .. (status and (" " .. tostring(status)) or "")
            else
                reason = "request failed: " .. tostring(code or status or "no response")
            end
            io.stderr:write("fetch failed (" .. reason .. "): " .. currentUrl .. "\n")
            return false, reason
        end
    end
    return false, "too many redirects (" .. tostring(maxRedirects) .. " hops)"
end

local function writeError(path, message)
    if not path or path == "" then return end
    local f = io.open(path, "wb")
    if f then
        f:write(message or "request failed")
        f:close()
    end
end

local mode = arg[1]
if mode == "get" then
    local ok, err = fetch(arg[2], arg[3], arg[4])
    if not ok then ok, err = fetch(arg[2], arg[3], arg[4]) end
    if not ok then writeError(arg[5], err) end
    os.exit(ok and 0 or 1)
elseif mode == "batch" then
    local failures = 0
    for line in io.lines(arg[2]) do
        local url, out, ref = line:match("^(.-)\t([^\t\r\n]+)\t?(.*)$")
        if url and out then
            ref = ref and ref:match("^%s*(.-)%s*$") or nil
            if not (fetch(url, out, ref) or fetch(url, out, ref)) then
                failures = failures + 1
            end
        end
    end
    os.exit(failures == 0 and 0 or 1)
else
    io.stderr:write("usage: fetch.lua get <url> <out> [referer] [errfile] | batch <list>\n")
    os.exit(2)
end
