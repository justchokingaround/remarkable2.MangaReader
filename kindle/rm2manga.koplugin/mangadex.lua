--[[--
MangaDex v5 API client & CBZ packager for rm2manga KOReader plugin.
--]]--

local JSON = require("json")
local Archiver = require("ffi/archiver")
local http = require("socket.http")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local ltn12 = require("ltn12")
local socket = require("socket")
local util = require("util")

local MangaDex = {
    api_base = "https://api.mangadex.org",
    user_agent = "rm2manga/1.0 (Kindle Oasis; KOReader)",
}

-- URL encode helper
function MangaDex:urlEncode(str)
    if not str then return "" end
    str = string.gsub(str, "\n", "\r\n")
    str = string.gsub(str, "([^%w %-%_%.%~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end)
    str = string.gsub(str, " ", "+")
    return str
end

-- Sanitize filename for FAT32 filesystem
function MangaDex:sanitizeFilename(name)
    if not name or name == "" then return "Unknown" end
    -- Replace illegal FAT32 characters \ / : * ? " < > |
    local clean = name:gsub("[\\/:*?\"<>|]", "_")
    -- Trim whitespace and dots
    clean = clean:gsub("^%s+", ""):gsub("%s+$", ""):gsub("%.+$", "")
    if clean == "" then clean = "Unknown" end
    return clean
end

-- HTTP GET request helper
function MangaDex:httpGet(url, timeout)
    local sink = {}
    http.TIMEOUT = timeout or 25

    local request = {
        url = url,
        method = "GET",
        headers = {
            ["User-Agent"] = self.user_agent,
            ["Accept"] = "application/json",
        },
        sink = ltn12.sink.table(sink),
        redirect = true,
    }

    local _, code, headers = http.request(request)

    if not code or code ~= 200 then
        logger.warn("MangaDex:httpGet failed: code", code, "url", url)
        return nil, "HTTP " .. tostring(code)
    end

    return table.concat(sink), nil
end

-- Download raw bytes (for images)
function MangaDex:httpDownloadBytes(url, timeout)
    local sink = {}
    http.TIMEOUT = timeout or 35

    local request = {
        url = url,
        method = "GET",
        headers = {
            ["User-Agent"] = self.user_agent,
        },
        sink = ltn12.sink.table(sink),
        redirect = true,
    }

    local _, code, headers = http.request(request)

    if not code or code ~= 200 then
        return nil, "HTTP " .. tostring(code)
    end

    return table.concat(sink), nil
end

-- Helper to pick best localized title
function MangaDex:pickTitle(titleObj, altTitles)
    if type(titleObj) == "string" then return titleObj end
    if type(titleObj) == "table" then
        if titleObj.en and titleObj.en ~= "" then return titleObj.en end
        if type(altTitles) == "table" then
            for _, alt in ipairs(altTitles) do
                if type(alt) == "table" and alt.en and alt.en ~= "" then
                    return alt.en
                end
            end
        end
        if titleObj["ja-ro"] and titleObj["ja-ro"] ~= "" then return titleObj["ja-ro"] end
        if titleObj["ko-ro"] and titleObj["ko-ro"] ~= "" then return titleObj["ko-ro"] end
        for _, v in pairs(titleObj) do
            if type(v) == "string" and v ~= "" then return v end
        end
    end
    return "Unknown Title"
end

-- Helper to pick localized description
function MangaDex:pickDescription(descObj)
    if type(descObj) == "string" then return descObj end
    if type(descObj) == "table" then
        if descObj.en and descObj.en ~= "" then return descObj.en end
        for _, v in pairs(descObj) do
            if type(v) == "string" and v ~= "" then return v end
        end
    end
    return ""
end

-- Parse a MangaDex manga data object
function MangaDex:parseManga(data)
    local attrs = data.attributes or {}
    local id = data.id or ""
    local title = self:pickTitle(attrs.title, attrs.altTitles)
    local desc = self:pickDescription(attrs.description)
    local status = attrs.status or ""
    local year = attrs.year or ""

    local author = ""
    local coverFile = ""
    if type(data.relationships) == "table" then
        for _, rel in ipairs(data.relationships) do
            if rel.type == "author" or rel.type == "artist" then
                if author == "" and rel.attributes and rel.attributes.name then
                    author = rel.attributes.name
                end
            elseif rel.type == "cover_art" then
                if rel.attributes and rel.attributes.fileName then
                    coverFile = rel.attributes.fileName
                end
            end
        end
    end

    local cover_url = ""
    if coverFile ~= "" and id ~= "" then
        cover_url = "https://uploads.mangadex.org/covers/" .. id .. "/" .. coverFile .. ".256.jpg"
    end

    return {
        id = id,
        title = title,
        description = desc,
        author = author,
        status = status,
        year = year,
        cover_url = cover_url,
    }
end

-- Search or browse manga list
function MangaDex:queryManga(query, mode, limit, offset)
    limit = limit or 20
    offset = offset or 0

    local url = self.api_base .. "/manga?limit=" .. tostring(limit)
                .. "&offset=" .. tostring(offset)
                .. "&includes[]=cover_art&includes[]=author"
                .. "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica"
                .. "&hasAvailableChapters=true"
                .. "&availableTranslatedLanguage[]=en"

    if mode == "latest" then
        url = url .. "&order[latestUploadedChapter]=desc"
    elseif mode == "popular" or not query or query == "" then
        url = url .. "&order[followedCount]=desc"
    else
        url = url .. "&title=" .. self:urlEncode(query) .. "&order[relevance]=desc"
    end

    local body, err = self:httpGet(url, 25)
    if not body then return nil, err end

    local ok, root = pcall(JSON.decode, body)
    if not ok or not root or type(root.data) ~= "table" then
        return nil, "Invalid JSON response from MangaDex"
    end

    local results = {}
    for _, item in ipairs(root.data) do
        table.insert(results, self:parseManga(item))
    end
    return results, nil, root.total or #results
end

-- Get chapter feed for a manga
function MangaDex:getChapters(mangaId, limit, offset)
    limit = limit or 100
    offset = offset or 0

    local url = self.api_base .. "/manga/" .. self:urlEncode(mangaId) .. "/feed?"
                .. "translatedLanguage[]=en"
                .. "&limit=" .. tostring(limit)
                .. "&offset=" .. tostring(offset)
                .. "&order[chapter]=asc&order[volume]=asc"
                .. "&includes[]=scanlation_group"
                .. "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica"

    local body, err = self:httpGet(url, 30)
    if not body then return nil, err end

    local ok, root = pcall(JSON.decode, body)
    if not ok or not root or type(root.data) ~= "table" then
        return nil, "Invalid chapter feed from MangaDex"
    end

    local chapters = {}
    for _, ch in ipairs(root.data) do
        local attrs = ch.attributes or {}
        -- Skip external-only chapters (e.g. MangaPlus links with 0 hosted pages)
        if not (attrs.externalUrl and attrs.externalUrl ~= "" and (attrs.pages or 0) <= 0) then
            local chNum = attrs.chapter or ""
            local title = attrs.title or ""
            local scanlator = ""
            if type(ch.relationships) == "table" then
                for _, rel in ipairs(ch.relationships) do
                    if rel.type == "scanlation_group" and rel.attributes and rel.attributes.name then
                        scanlator = rel.attributes.name
                        break
                    end
                end
            end

            table.insert(chapters, {
                id = ch.id,
                chapter_num = chNum,
                title = title,
                pages = attrs.pages or 0,
                scanlator = scanlator,
                publish_at = attrs.publishAt or "",
            })
        end
    end

    return chapters, nil, root.total or #chapters
end

-- Download chapter pages and pack directly into a CBZ archive
-- Returns: success (bool), path or error (string)
function MangaDex:downloadChapterCbz(chapter, mangaTitle, options, progressCb, cancelCheck)
    options = options or {}
    local useDataSaver = options.useDataSaver ~= false -- default to true for speed on e-ink

    -- 1. Contact MangaDex At-Home server
    local atHomeUrl = self.api_base .. "/at-home/server/" .. self:urlEncode(chapter.id)
    local atHomeBody, err = self:httpGet(atHomeUrl, 25)
    if not atHomeBody then
        return false, "Failed to reach MangaDex image server: " .. tostring(err)
    end

    local ok, root = pcall(JSON.decode, atHomeBody)
    if not ok or not root or root.result ~= "ok" or not root.chapter then
        return false, "MangaDex image server error"
    end

    local baseUrl = root.baseUrl or ""
    local hash = root.chapter.hash or ""
    local dataArr = root.chapter.data or {}
    local saverArr = root.chapter.dataSaver or {}

    local pages = (useDataSaver and #saverArr > 0) and saverArr or dataArr
    local modeDir = (useDataSaver and #saverArr > 0) and "data-saver" or "data"

    if baseUrl == "" or hash == "" or #pages == 0 then
        return false, "No downloadable pages found for this chapter"
    end

    -- 2. Prepare paths
    local baseDir = options.destDir or "/mnt/us/manga"
    local safeManga = self:sanitizeFilename(mangaTitle)
    local mangaDir = baseDir .. "/" .. safeManga
    lfs.mkdir(baseDir)
    lfs.mkdir(mangaDir)

    local chLabel = chapter.chapter_num ~= "" and ("Ch." .. chapter.chapter_num) or "Chapter"
    local titlePart = (chapter.title and chapter.title ~= "") and (" - " .. self:sanitizeFilename(chapter.title)) or ""
    local cbzFilename = chLabel .. titlePart .. ".cbz"
    local finalPath = mangaDir .. "/" .. cbzFilename
    local partPath = mangaDir .. "/" .. cbzFilename .. ".part"

    -- If already downloaded, return immediately
    local attr = lfs.attributes(finalPath)
    if attr and attr.size and attr.size > 1024 then
        return true, finalPath
    end

    -- 3. Open Archiver Writer for .part
    os.remove(partPath)
    local writer = Archiver.Writer:new()
    local openOk = writer:open(partPath, "zip")
    if not openOk then
        return false, "Failed to create CBZ archive: " .. tostring(writer.err)
    end

    local totalPages = #pages
    local downloadedCount = 0

    for i, pageFileName in ipairs(pages) do
        if cancelCheck and cancelCheck() then
            writer:close()
            os.remove(partPath)
            return false, "Download cancelled"
        end

        local pageUrl = baseUrl .. "/" .. modeDir .. "/" .. hash .. "/" .. pageFileName
        local imgData, dlErr = self:httpDownloadBytes(pageUrl, 40)

        -- Fallback to original quality if data-saver failed
        if not imgData and modeDir == "data-saver" and dataArr[i] then
            local fallbackUrl = baseUrl .. "/data/" .. hash .. "/" .. dataArr[i]
            imgData, dlErr = self:httpDownloadBytes(fallbackUrl, 40)
        end

        if not imgData then
            writer:close()
            os.remove(partPath)
            return false, "Failed downloading page " .. i .. " of " .. totalPages .. " (" .. tostring(dlErr) .. ")"
        end

        -- Determine extension
        local ext = "jpg"
        local dot = pageFileName:match("^.+%.(%w+)$")
        if dot then ext = dot:lower() end

        local entryName = string.format("%04d.%s", i, ext)
        local addOk = writer:addFileFromMemory(entryName, imgData)
        if not addOk then
            writer:close()
            os.remove(partPath)
            return false, "Failed writing page " .. i .. " to CBZ"
        end

        downloadedCount = downloadedCount + 1
        if progressCb then
            progressCb(downloadedCount, totalPages)
        end
    end

    writer:close()

    -- 4. Atomically rename .part to final .cbz
    os.remove(finalPath)
    local renameOk = os.rename(partPath, finalPath)
    if not renameOk then
        return false, "Failed saving final CBZ file"
    end

    logger.info("rm2manga: Downloaded", finalPath, "totalPages:", totalPages)
    return true, finalPath
end

return MangaDex
