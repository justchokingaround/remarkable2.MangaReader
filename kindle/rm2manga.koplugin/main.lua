--[[--
rm2manga: Native Manga Reader & Downloader for KOReader on Kindle Oasis.
Powered by MangaDex v5.
--]]--

local BD = require("ui/bidi")
local ConfirmBox = require("ui/widget/confirmbox")
local DataStorage = require("datastorage")
local Dispatcher = require("dispatcher")
local InfoMessage = require("ui/widget/infomessage")
local InputDialog = require("ui/widget/inputdialog")
local LuaSettings = require("luasettings")
local Menu = require("ui/widget/menu")
local NetworkMgr = require("ui/network/manager")
local Trapper = require("ui/trapper")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local util = require("util")
local _ = require("gettext")
local T = require("ffi/util").template

local MangaDex = require("mangadex")

local Rm2Manga = WidgetContainer:extend{
    name = "rm2manga",
    settings_file = DataStorage:getSettingsDir() .. "/rm2manga.lua",
    settings = nil,
    use_data_saver = true,
    manga_root_dir = "/mnt/us/manga",
}

function Rm2Manga:init()
    self:loadSettings()
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
    lfs.mkdir(self.manga_root_dir)
end

function Rm2Manga:loadSettings()
    self.settings = LuaSettings:open(self.settings_file)
    self.use_data_saver = self.settings:readSetting("use_data_saver", true)
end

function Rm2Manga:saveSettings()
    if self.settings then
        self.settings:saveSetting("use_data_saver", self.use_data_saver)
        self.settings:flush()
    end
end

function Rm2Manga:onDispatcherRegisterActions()
    Dispatcher:registerAction("rm2manga_open", {
        category = "none",
        event = "ShowRm2MangaMenu",
        title = _("rm2manga: Manga Reader"),
        general = true,
    })
    Dispatcher:registerAction("rm2manga_search", {
        category = "none",
        event = "SearchRm2Manga",
        title = _("rm2manga: Search Manga"),
        general = true,
    })
end

function Rm2Manga:addToMainMenu(menu_items)
    menu_items.rm2manga = {
        text = _("rm2manga (Manga Reader)"),
        sorting_hint = "more_tools",
        sub_item_table_func = function()
            return self:getSubMenuItems()
        end,
    }
end

function Rm2Manga:getSubMenuItems()
    return {
        {
            text = _("Search Manga..."),
            callback = function()
                self:promptSearch()
            end,
        },
        {
            text = _("Popular Manga"),
            callback = function()
                self:browseCatalog("popular", _("Popular Manga"))
            end,
        },
        {
            text = _("Latest Manga Updates"),
            callback = function()
                self:browseCatalog("latest", _("Latest Manga"))
            end,
        },
        {
            text = _("My Manga Library"),
            callback = function()
                self:openMangaFolder()
            end,
        },
        {
            text = _("Data Saver Mode (faster downloads)"),
            checked_func = function()
                return self.use_data_saver
            end,
            callback = function()
                self.use_data_saver = not self.use_data_saver
                self:saveSettings()
            end,
        },
    }
end

-- Open downloaded manga directory in KOReader file manager
function Rm2Manga:openMangaFolder()
    lfs.mkdir(self.manga_root_dir)
    if self.ui.file_chooser then
        self.ui.file_chooser:changeToPath(self.manga_root_dir)
    elseif self.ui.openFile then
        -- If in reader mode, open the directory
        local FileManager = require("apps/filemanager/filemanager")
        local fm = FileManager:showFiles(self.manga_root_dir)
        UIManager:show(fm)
    end
end

-- Show search keyboard dialog
function Rm2Manga:promptSearch()
    local search_dialog
    search_dialog = InputDialog:new{
        title = _("Search Manga on MangaDex"),
        input = "",
        buttons = {
            {
                {
                    text = _("Cancel"),
                    id = "close",
                    callback = function()
                        UIManager:close(search_dialog)
                    end,
                },
                {
                    text = _("Search"),
                    is_enter_default = true,
                    callback = function()
                        local query = search_dialog:getInputText()
                        UIManager:close(search_dialog)
                        if query and query:match("%S") then
                            self:searchManga(query)
                        end
                    end,
                },
            },
        },
    }
    UIManager:show(search_dialog)
    search_dialog:onShowKeyboard()
end

-- Run search with loading notification
function Rm2Manga:searchManga(query)
    local info = InfoMessage:new{ text = _("Searching MangaDex for '") .. query .. "'..." }
    UIManager:show(info)

    Trapper:wrap(function()
        local results, err = MangaDex:queryManga(query, "search", 24, 0)
        UIManager:close(info)

        if not results or #results == 0 then
            UIManager:show(InfoMessage:new{
                text = err or (_("No manga found matching '") .. query .. "'"),
                timeout = 4,
            })
            return
        end

        self:showMangaList(_("Results for: ") .. query, results)
    end)
end

-- Browse popular or latest manga
function Rm2Manga:browseCatalog(mode, title)
    local info = InfoMessage:new{ text = _("Loading ") .. title .. "..." }
    UIManager:show(info)

    Trapper:wrap(function()
        local results, err = MangaDex:queryManga("", mode, 24, 0)
        UIManager:close(info)

        if not results or #results == 0 then
            UIManager:show(InfoMessage:new{
                text = err or _("Failed to load manga feed. Please check Wi-Fi."),
                timeout = 4,
            })
            return
        end

        self:showMangaList(title, results)
    end)
end

-- Display list of manga results
function Rm2Manga:showMangaList(title, mangaList)
    local item_table = {}

    for _, manga in ipairs(mangaList) do
        local sub = ""
        if manga.author and manga.author ~= "" then
            sub = manga.author
        end
        if manga.status and manga.status ~= "" then
            sub = (sub ~= "" and (sub .. " • ") or "") .. manga.status:upper()
        end

        table.insert(item_table, {
            text = manga.title,
            mandatory = sub,
            callback = function()
                self:showMangaDetails(manga)
            end,
        })
    end

    local manga_menu = Menu:new{
        title = title,
        item_table = item_table,
        is_borderless = true,
        is_popout = false,
        title_bar_fm_style = true,
    }
    UIManager:show(manga_menu)
end

-- Show manga details and chapters
function Rm2Manga:showMangaDetails(manga)
    local info = InfoMessage:new{ text = _("Fetching chapters for ") .. manga.title .. "..." }
    UIManager:show(info)

    Trapper:wrap(function()
        local chapters, err = MangaDex:getChapters(manga.id, 100, 0)
        UIManager:close(info)

        if not chapters or #chapters == 0 then
            UIManager:show(InfoMessage:new{
                text = err or _("No English chapters available for this manga."),
                timeout = 4,
            })
            return
        end

        self:showChapterList(manga, chapters)
    end)
end

-- Display chapter list
function Rm2Manga:showChapterList(manga, chapters)
    local item_table = {}
    local safeManga = MangaDex:sanitizeFilename(manga.title)
    local mangaDir = self.manga_root_dir .. "/" .. safeManga

    for _, ch in ipairs(chapters) do
        local label = ch.chapter_num ~= "" and ("Ch. " .. ch.chapter_num) or "Chapter"
        if ch.title and ch.title ~= "" then
            label = label .. ": " .. ch.title
        end

        -- Check if already downloaded on disk
        local cbzName = (ch.chapter_num ~= "" and ("Ch." .. ch.chapter_num) or "Chapter")
        if ch.title and ch.title ~= "" then
            cbzName = cbzName .. " - " .. MangaDex:sanitizeFilename(ch.title)
        end
        local expectedPath = mangaDir .. "/" .. cbzName .. ".cbz"
        local attr = lfs.attributes(expectedPath)
        local isDownloaded = attr and attr.size and attr.size > 1024

        local mandatoryText = isDownloaded and _("✓ Downloaded") or (tostring(ch.pages) .. " pgs")
        if ch.scanlator and ch.scanlator ~= "" then
            mandatoryText = mandatoryText .. " • " .. ch.scanlator
        end

        table.insert(item_table, {
            text = label,
            mandatory = mandatoryText,
            callback = function()
                self:onChapterSelected(manga, ch, expectedPath, isDownloaded)
            end,
        })
    end

    local ch_menu = Menu:new{
        title = manga.title .. _(" (Chapters)"),
        item_table = item_table,
        is_borderless = true,
        is_popout = false,
        title_bar_fm_style = true,
    }
    UIManager:show(ch_menu)
end

-- Handle chapter tap: read or download
function Rm2Manga:onChapterSelected(manga, chapter, filePath, isDownloaded)
    if isDownloaded then
        local confirm = ConfirmBox:new{
            text = T(_("Chapter already downloaded:\n%1\n\nRead chapter now?"), BD.filepath(filePath)),
            ok_text = _("Read now"),
            ok_callback = function()
                self:openDocument(filePath)
            end,
        }
        UIManager:show(confirm)
        return
    end

    -- Prompt download confirmation
    local prompt = ConfirmBox:new{
        text = T(_("Download %1\nPages: %2\nSave to: %3?"),
            (chapter.chapter_num ~= "" and ("Chapter " .. chapter.chapter_num) or "Chapter"),
            chapter.pages,
            BD.filepath(filePath)),
        ok_text = _("Download"),
        ok_callback = function()
            self:startDownload(manga, chapter)
        end,
    }
    UIManager:show(prompt)
end

-- Start background chapter download
function Rm2Manga:startDownload(manga, chapter)
    local cancelState = { is_cancelled = false }
    local progressInfo = InfoMessage:new{
        text = _("Downloading chapter... (tap to cancel)"),
        dismissable = true,
        callback = function()
            cancelState.is_cancelled = true
        end,
    }
    UIManager:show(progressInfo)

    Trapper:wrap(function()
        local function onProgress(current, total)
            progressInfo:setText(T(_("Downloading page %1 of %2... (tap to cancel)"), current, total))
            UIManager:forceRePaint()
        end

        local function cancelCheck()
            return cancelState.is_cancelled
        end

        local ok, result = MangaDex:downloadChapterCbz(
            chapter,
            manga.title,
            {
                destDir = self.manga_root_dir,
                useDataSaver = self.use_data_saver,
            },
            onProgress,
            cancelCheck
        )

        UIManager:close(progressInfo)

        if not ok then
            UIManager:show(InfoMessage:new{
                text = _("Download failed: ") .. tostring(result),
                timeout = 5,
            })
            return
        end

        -- Download successful! Prompt to read immediately
        local readConfirm = ConfirmBox:new{
            text = T(_("Download complete!\nSaved to:\n%1\n\nWould you like to read now?"), BD.filepath(result)),
            ok_text = _("Read now"),
            ok_callback = function()
                self:openDocument(result)
            end,
        }
        UIManager:nextTick(function()
            UIManager:show(readConfirm)
        end)
    end)
end

-- Open document in KOReader reader engine
function Rm2Manga:openDocument(filePath)
    if self.ui.document then
        self.ui:switchDocument(filePath)
    elseif self.ui.openFile then
        self.ui:openFile(filePath)
    else
        local ReaderUI = require("apps/reader/readerui")
        ReaderUI:showReader(filePath)
    end
end

return Rm2Manga
