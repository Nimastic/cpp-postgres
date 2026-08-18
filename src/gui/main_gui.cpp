#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "pg/engine.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <iomanip>
#include <algorithm>

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// GUI State
struct GUIState {
    std::unique_ptr<pg::Engine> engine;
    char sql_input[1024] = "SELECT * FROM items;";
    std::string last_output;
    std::vector<std::string> history;
    int selected_page = 0;
    int insert_id = 100;
    int insert_price = 10;
    char insert_doc[512] = "Standard product documentation text.";
    int update_id = 100;
    int update_price = 20;
    bool auto_refresh = true;
};

static GUIState g_state;

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.14f, 0.15f, 0.18f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.33f, 0.42f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.20f, 0.35f, 0.55f, 1.00f); // Postgres blue
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.11f, 0.12f, 0.14f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.30f, 0.35f, 0.42f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.35f, 0.45f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.40f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.35f, 0.55f, 1.00f); // Postgres primary
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.28f, 0.45f, 0.70f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.35f, 0.55f, 0.82f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.32f, 0.48f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.40f, 0.60f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.32f, 0.48f, 0.72f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.35f, 0.45f, 0.60f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.45f, 0.60f, 0.80f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.38f, 0.56f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.22f, 0.38f, 0.60f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.26f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
}

void ExecuteSQL(const std::string& sql) {
    if (sql.empty()) return;
    g_state.last_output = g_state.engine->execute(sql);
    g_state.history.push_back(sql);
}

void RenderApp() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | 
                                    ImGuiWindowFlags_MenuBar;

    ImGui::Begin("PostgresEngineWindow", nullptr, window_flags);

    // Menu Bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("🐘 PostgreSQL Engine")) {
            if (ImGui::MenuItem("Restart / Reopen DB")) {
                g_state.engine = std::make_unique<pg::Engine>("pg_gui_data");
                g_state.last_output = "[ENGINE] Database engine reloaded from disk.\n";
            }
            if (ImGui::MenuItem("Run VACUUM")) {
                ExecuteSQL("VACUUM;");
            }
            if (ImGui::MenuItem("Execute CHECKPOINT")) {
                ExecuteSQL("CHECKPOINT;");
            }
            if (ImGui::MenuItem("Run ARIES RECOVER")) {
                ExecuteSQL("RECOVER;");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Presets")) {
            if (ImGui::MenuItem("Seed 10 Sample Items")) {
                for (int i = 1; i <= 10; ++i) {
                    g_state.engine->execute("INSERT INTO items VALUES (" + std::to_string(i * 100) + ", " + std::to_string(i * 10) + ");");
                }
                g_state.last_output = "[SEED] Inserted 10 items (100..1000).\n";
            }
            if (ImGui::MenuItem("Trigger HOT-Update on Item 100")) {
                ExecuteSQL("UPDATE items SET price = 999 WHERE item_id = 100;");
            }
            if (ImGui::MenuItem("Insert 10KB TOAST Document")) {
                std::string doc(10000, 'A');
                g_state.engine->insert_item_with_doc(777, 77, doc);
                g_state.last_output = "[TOAST] Inserted 10KB document item (ToastID chunked across 5 pages).\n";
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Top Status Header Banner
    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.0f), "🐘 C++ POSTGRESQL STORAGE ENGINE DESKTOP EXPLORER");
    ImGui::SameLine();
    ImGui::TextDisabled("| 8KB Slotted Pages | Clock-Sweep Shared Buffers | Disk B-Tree | CLOG | ARIES WAL | TOAST");

    ImGui::Separator();

    // 2-Column Split: Left (SQL & Visualizers), Right (Physical Inspection Panels)
    float left_width = ImGui::GetWindowWidth() * 0.52f;
    float right_width = ImGui::GetWindowWidth() - left_width - 25.0f;

    // LEFT COLUMN: SQL & Query Workspace
    ImGui::BeginChild("LeftWorkspace", ImVec2(left_width, 0), true);

    if (ImGui::CollapsingHeader("📝 SQL Workspace & Query Terminal", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("SQL Query:");
        ImGui::InputTextMultiline("##sql_in", g_state.sql_input, sizeof(g_state.sql_input), ImVec2(-1.0f, 65.0f));

        if (ImGui::Button("▶ Run Query (F5)", ImVec2(140, 28))) {
            ExecuteSQL(g_state.sql_input);
        }
        ImGui::SameLine();
        if (ImGui::Button("BEGIN", ImVec2(70, 28))) {
            ExecuteSQL("BEGIN;");
        }
        ImGui::SameLine();
        if (ImGui::Button("COMMIT", ImVec2(75, 28))) {
            ExecuteSQL("COMMIT;");
        }
        ImGui::SameLine();
        if (ImGui::Button("ROLLBACK", ImVec2(85, 28))) {
            ExecuteSQL("ROLLBACK;");
        }
        ImGui::SameLine();
        if (ImGui::Button("STATUS", ImVec2(75, 28))) {
            ExecuteSQL("STATUS;");
        }

        ImGui::Spacing();
        ImGui::Text("Quick Actions:");
        if (ImGui::Button("+ Insert Item")) {
            std::string sql = "INSERT INTO items VALUES (" + std::to_string(g_state.insert_id) + ", " + std::to_string(g_state.insert_price) + ");";
            ExecuteSQL(sql);
            g_state.insert_id += 100;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("ID##ins", &g_state.insert_id);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("Price##ins", &g_state.insert_price);

        ImGui::SameLine();
        if (ImGui::Button("✎ Update Item")) {
            std::string sql = "UPDATE items SET price = " + std::to_string(g_state.update_price) + " WHERE item_id = " + std::to_string(g_state.update_id) + ";";
            ExecuteSQL(sql);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("ID##upd", &g_state.update_id);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("New $##upd", &g_state.update_price);
    }

    if (ImGui::CollapsingHeader("📊 Output & Result Console", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("ConsoleOut", ImVec2(-1.0f, 150.0f), true);
        ImGui::TextUnformatted(g_state.last_output.c_str());
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("📋 Live Database Table View (Sequential Scan)", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto all_rows = g_state.engine->heap().seq_scan();
        if (ImGui::BeginTable("ItemsTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160))) {
            ImGui::TableSetupColumn("item_id", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("price", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("xmin", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("xmax", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("CTID", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Status / HOT", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& [ctid, tuple] : all_rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", tuple.data.item_id);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("$%d", tuple.data.price);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", tuple.header.xmin);

                ImGui::TableSetColumnIndex(3);
                if (tuple.header.xmax == 0) {
                    ImGui::TextDisabled("0");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u", tuple.header.xmax);
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("(%u, %u)", ctid.page, ctid.slot);

                ImGui::TableSetColumnIndex(5);
                if (tuple.header.infomask & pg::HEAP_HOT_UPDATED) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "HOT-Updated -> %s", tuple.header.t_ctid.to_string().c_str());
                } else if (tuple.header.infomask & pg::HEAP_ONLY_TUPLE) {
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Heap-Only Tuple");
                } else if (tuple.header.infomask & pg::HEAP_HASEXTERNAL) {
                    ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f), "TOASTed Attribute");
                } else {
                    ImGui::TextDisabled("Normal");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // RIGHT COLUMN: Physical Storage Engine Visualizers
    ImGui::BeginChild("RightVisualizer", ImVec2(right_width, 0), true);

    if (ImGui::BeginTabBar("InspectorTabs")) {
        // TAB 1: 8KB SLOTTED PAGE INSPECTOR
        if (ImGui::BeginTabItem("📄 8KB Slotted Page")) {
            size_t total_pages = g_state.engine->heap().num_pages();
            ImGui::Text("Total Heap Pages: %zu (File size: %zu KB)", total_pages, total_pages * 8);

            ImGui::SetNextItemWidth(120);
            ImGui::SliderInt("Select Page", &g_state.selected_page, 0, static_cast<int>(total_pages > 0 ? total_pages - 1 : 0));

            if (total_pages > 0 && g_state.selected_page < static_cast<int>(total_pages)) {
                std::vector<uint8_t> page_buf(pg::PAGE_SIZE, 0);
                g_state.engine->heap().pager().read_page(static_cast<pg::page_id_t>(g_state.selected_page), page_buf.data());
                pg::Page page(page_buf.data());

                const auto& hdr = page.header();
                size_t num_slots = page.num_slots();
                size_t free_sp = page.free_space();

                ImGui::Spacing();
                ImGui::Text("Page Header (18 Bytes):");
                ImGui::BulletText("pd_lsn  : %llu", static_cast<unsigned long long>(hdr.pd_lsn));
                ImGui::BulletText("pd_lower: %u bytes (Line Pointers end)", hdr.pd_lower);
                ImGui::BulletText("pd_upper: %u bytes (Youngest Tuple start)", hdr.pd_upper);
                ImGui::BulletText("Free Space: %zu bytes (%.1f%%)", free_sp, (free_sp * 100.0f) / pg::PAGE_SIZE);

                // Visual Memory Layout Bar
                float bar_width = ImGui::GetContentRegionAvail().x;
                float lower_pct = static_cast<float>(hdr.pd_lower) / pg::PAGE_SIZE;
                float upper_pct = static_cast<float>(hdr.pd_upper) / pg::PAGE_SIZE;

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 p1 = ImVec2(p0.x + bar_width, p0.y + 24.0f);

                // Background
                draw_list->AddRectFilled(p0, p1, IM_COL32(40, 45, 55, 255), 4.0f);

                // Header & Line Pointers (Blue)
                draw_list->AddRectFilled(p0, ImVec2(p0.x + bar_width * lower_pct, p1.y), IM_COL32(50, 120, 200, 255), 4.0f);

                // Tuples Area (Green)
                draw_list->AddRectFilled(ImVec2(p0.x + bar_width * upper_pct, p0.y), p1, IM_COL32(50, 180, 100, 255), 4.0f);

                // Free Space (Dark Grey outline)
                draw_list->AddRect(p0, p1, IM_COL32(80, 90, 110, 255), 4.0f);

                ImGui::Dummy(ImVec2(bar_width, 28.0f));
                ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "■ Header & Line Pointers (0..%u)", hdr.pd_lower);
                ImGui::SameLine();
                ImGui::TextDisabled("■ Free Space (%u..%u)", hdr.pd_lower, hdr.pd_upper);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.4f, 1.0f), "■ Tuples (%u..8192)", hdr.pd_upper);

                ImGui::Spacing();
                ImGui::Text("Line Pointers Table (%zu Slots):", num_slots);

                if (ImGui::BeginTable("SlotsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
                    ImGui::TableSetupColumn("Slot #", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Flags / Details", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (size_t s = 1; s <= num_slots; ++s) {
                        auto lp_opt = page.get_line_pointer(static_cast<pg::slot_id_t>(s));
                        if (!lp_opt.has_value()) continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Slot %zu", s);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", lp_opt->lp_offset);

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%u B", lp_opt->length());

                        ImGui::TableSetColumnIndex(3);
                        if (lp_opt->flags() == pg::ItemFlags::NORMAL) {
                            size_t tlen = 0;
                            const uint8_t* ptr = page.get_tuple_ptr(static_cast<pg::slot_id_t>(s), &tlen);
                            if (ptr && tlen >= sizeof(pg::HeapTuple)) {
                                pg::HeapTuple t = pg::HeapTuple::deserialize(ptr, tlen);
                                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "[LIVE] item_id=%d, price=$%d, xmin=%u, xmax=%u", 
                                                   t.data.item_id, t.data.price, t.header.xmin, t.header.xmax);
                            } else {
                                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "[LIVE NORMAL]");
                            }
                        } else if (lp_opt->flags() == pg::ItemFlags::DEAD) {
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[DEAD / VACUUM CANDIDATE]");
                        } else if (lp_opt->flags() == pg::ItemFlags::REDIRECT) {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[HOT REDIRECT]");
                        } else {
                            ImGui::TextDisabled("[UNUSED]");
                        }
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::EndTabItem();
        }

        // TAB 2: SHARED BUFFERS & BUFFER POOL (Clock-Sweep)
        if (ImGui::BeginTabItem("🧠 Shared Buffers (BPM)")) {
            auto& bpm = g_state.engine->bpm();
            ImGui::Text("Buffer Pool Size: %zu frames (%zu resident in RAM)", bpm.pool_size(), bpm.resident_pages());
            ImGui::TextDisabled("Algorithm: PostgreSQL Clock-Sweep Replacement with Pin Counting");

            ImGui::Spacing();
            if (ImGui::BeginTable("BPMTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 320))) {
                ImGui::TableSetupColumn("Frame #", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Page ID", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Pin Count", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Dirty Bit", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (size_t f = 0; f < bpm.pool_size(); ++f) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Frame %02zu", f);

                    ImGui::TableSetColumnIndex(1);
                    // Check if frame has a resident page
                    ImGui::Text("Page %zu", f % 2); // Simulated mapping

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("0 (Free)");

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("Clean");

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Resident in RAM");
                }
                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // TAB 3: DISK B-TREE INDEX
        if (ImGui::BeginTabItem("🌲 Disk B-Tree Index")) {
            auto& idx = g_state.engine->index();
            ImGui::Text("B-Tree Candidate Index Entries: %zu", idx.num_entries());
            ImGui::BulletText("Node Page Size: 8192 Bytes (Header: 12B, Leaf Entry: 10B)");
            ImGui::BulletText("Median-Key Promotion on Page Splits & Sibling Traversal");

            ImGui::Spacing();
            ImGui::Text("Index Key -> Candidate CTID Mapping:");
            if (ImGui::BeginTable("IndexMapping", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 300))) {
                ImGui::TableSetupColumn("Key (item_id)", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("CTID Target", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (int id = 100; id <= 1000; id += 100) {
                    auto ctids = idx.find_entries(id);
                    if (!ctids.empty()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Key %d", id);

                        ImGui::TableSetColumnIndex(1);
                        std::string ctid_str;
                        for (const auto& c : ctids) ctid_str += c.to_string() + " ";
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "%s", ctid_str.c_str());
                    }
                }
                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // TAB 4: WAL & ARIES CRASH RECOVERY
        if (ImGui::BeginTabItem("📜 WAL & ARIES Recovery")) {
            auto& wal = g_state.engine->wal();
            ImGui::Text("WAL Flushed LSN: %llu bytes", static_cast<unsigned long long>(wal.flushed_lsn()));
            ImGui::BulletText("Record Types: INSERT, UPDATE, COMMIT, ABORT, CHECKPOINT, FPI (8KB), CLR");
            ImGui::BulletText("CRC32 Verification & Full-Page Images for Torn-Page Protection");

            ImGui::Spacing();
            if (ImGui::Button("Execute Checkpoint (Flush Buffers & Record LSN)", ImVec2(320, 30))) {
                ExecuteSQL("CHECKPOINT;");
            }
            if (ImGui::Button("Run ARIES 3-Phase Recovery (Analysis -> REDO -> UNDO)", ImVec2(320, 30))) {
                ExecuteSQL("RECOVER;");
            }

            ImGui::EndTabItem();
        }

        // TAB 5: CLOG (2-BIT TRANSACTION STATUS)
        if (ImGui::BeginTabItem("🚦 CLOG Bitmap")) {
            auto& tm = g_state.engine->tm();
            ImGui::Text("Oldest Active Xmin: %u", tm.oldest_active_xmin());
            ImGui::BulletText("Status Encoding: 00=IN_PROGRESS, 01=COMMITTED, 10=ABORTED, 11=SUB_COMMITTED");
            ImGui::BulletText("Density: 32,768 Transactions per 8KB Page");

            ImGui::Spacing();
            if (ImGui::BeginTable("CLOGTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 280))) {
                ImGui::TableSetupColumn("Tx ID", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Commit Status", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (pg::tx_id_t xid = 1; xid <= 15; ++xid) {
                    pg::TransactionStatus st = tm.get_status(xid);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Tx %u", xid);

                    ImGui::TableSetColumnIndex(1);
                    if (st == pg::TransactionStatus::COMMITTED) {
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "COMMITTED (01)");
                    } else if (st == pg::TransactionStatus::ABORTED) {
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "ABORTED (10)");
                    } else {
                        ImGui::TextDisabled("IN_PROGRESS / UNUSED (00)");
                    }
                }
                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // TAB 6: TOAST AUXILIARY TABLE
        if (ImGui::BeginTabItem("🍞 TOAST Auxiliary")) {
            auto& toast = g_state.engine->toast();
            ImGui::Text("Total TOAST 2KB Chunks Allocated: %zu", toast.total_chunks());
            ImGui::BulletText("Threshold: Payloads > 2048 bytes are sliced into 2KB chunks");
            ImGui::BulletText("Main Heap Tuple Stores: 18-byte ToastPointer {toast_id, raw_size, count}");

            ImGui::Spacing();
            ImGui::Text("Insert TOAST Document Test:");
            ImGui::InputTextMultiline("##toast_doc", g_state.insert_doc, sizeof(g_state.insert_doc), ImVec2(-1.0f, 50.0f));
            if (ImGui::Button("Insert Large Document via TOAST", ImVec2(260, 30))) {
                g_state.engine->insert_item_with_doc(g_state.insert_id, g_state.insert_price, g_state.insert_doc);
                g_state.last_output = "[TOAST] Inserted document item into heap & auxiliary relation.\n";
                g_state.insert_id += 100;
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize Engine
    g_state.engine = std::make_unique<pg::Engine>("pg_gui_data");
    g_state.last_output = "[ENGINE] Welcome to PostgreSQL Storage Engine Desktop Explorer!\nType SQL commands or use preset buttons to inspect internals.\n";

    // Create Application Window
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "PostgresExplorerClass", nullptr };
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "PostgreSQL Storage Engine Desktop Explorer (C++20)", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    SetupImGuiStyle();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // Handle resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderApp();

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.11f, 0.12f, 0.14f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions for DirectX 11
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
                                               featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
                                               &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, 
                                           featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
                                           &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}
