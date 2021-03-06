#include <WinSock2.h>
#include "TradeWindow.h"

#include <GWCA\GWCA.h>
#include <GWCA\Managers\UIMgr.h>
#include <GWCA\Managers\ChatMgr.h>
#include <GWCA\Managers\GameThreadMgr.h>
#include <GWCA\Managers\MapMgr.h>
#include <Modules\Resources.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "logger.h"
#include "GuiUtils.h"
#include "GWToolbox.h"

#include <list>
#include <fstream>

using easywsclient::WebSocket;
using nlohmann::json;
using json_vec = std::vector<json>;

static const char ws_host[] = "wss://kamadan.decltype.org/ws/";

void TradeWindow::Initialize() {
	ToolboxWindow::Initialize();

	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		printf("WSAStartup Failed.\n");
		return;
	}

	messages = CircularBuffer<Message>(100);

	should_stop = false;
	worker = std::thread([this]() {
		while (!should_stop) {
			if (thread_jobs.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			} else {
				thread_jobs.front()();
				thread_jobs.pop();
			}
		}
	});

	if (print_game_chat) AsyncChatConnect();
}

void TradeWindow::Update(float delta) {
	if (!print_game_chat) return;

	if (ws_chat && ws_chat->getReadyState() == WebSocket::CLOSED) {
		delete ws_chat;
		ws_chat = nullptr;
	}

	// do not display trade chat while in kamadan AE district 1
	if (GW::Map::GetMapID() == GW::Constants::MapID::Kamadan_Jewel_of_Istan_outpost &&
		GW::Map::GetDistrict() == 1 &&
		GW::Map::GetRegion() == GW::Constants::Region::America) {
		if (ws_chat) ws_chat->close();
		return;
	}
	
	if (!ws_chat && !ws_chat_connecting) {
		AsyncChatConnect();
		return;
	}

	if (!ws_chat || ws_chat->getReadyState() != WebSocket::OPEN)
		return;

	ws_chat->poll();
	ws_chat->dispatch([this](const std::string& data) {
		char buffer[512];
		json res = json::parse(data.c_str());

		// We don't support queries in the chat
		if (res.find("query") != res.end())
			return;

		std::string msg = res["message"].get<std::string>();
		bool print_message = true;
		if (filter_alerts) {
			print_message = false; // filtered unless allowed by words
			for (auto& word : alert_words) {
				auto found = std::search(msg.begin(), msg.end(), word.begin(), word.end(), [](char c1, char c2) -> bool {
					return tolower(c1) == c2;
				});
				if (found != msg.end()) {
					print_message = true;
					break; // don't need to check other words
				}
			}
		}

		if (print_message) {
			std::string name = res["name"].get<std::string>();
			snprintf(buffer, sizeof(buffer), "<a=1>%s</a>: <c=#f96677><quote>%s", name.c_str(), msg.c_str());
			GW::Chat::WriteChat(GW::Chat::CHANNEL_TRADE, buffer);
		}
	});
}

TradeWindow::Message TradeWindow::parse_json_message(json js) {
	TradeWindow::Message msg;
	msg.name = js["name"].get<std::string>();
	msg.message = js["message"].get<std::string>();
	msg.timestamp = stoi(js["timestamp"].get<std::string>());
	return msg;
}

void TradeWindow::fetch() {
	if (ws_window == nullptr) return;
	if (ws_window->getReadyState() != WebSocket::OPEN) return;
	
	ws_window->poll();
	ws_window->dispatch([this](const std::string& data) {
		json res = json::parse(data.c_str());
		if (res.find("query") == res.end()) {
			// It's a new message
			Message msg = parse_json_message(res);
			messages.add(msg);
		} else {
			search_pending = false;
			if (res["num_results"].get<std::string>() == "0")
				return;
			json_vec results = res["results"].get<json_vec>();
			messages.clear();
			for (auto it = results.rbegin(); it != results.rend(); it++) {
				Message msg = parse_json_message(*it);
				messages.add(msg);
			}
		}
	});
}

void TradeWindow::search(std::string query) {
	if (!ws_window || ws_window->getReadyState() != WebSocket::OPEN)
		return;

	// for now we won't allow to enqueue more than 1 search, it shouldn't change anything because how fast we get the answers
	if (search_pending)
		return;

	search_pending = true;

	/*
	 * The protocol is the following:
	 *  - From a connected web socket, we send a Json formated packet with the format
	 *  {
	 *   "query":  str($query$),
	 *   "offset": int($page$),
	 *   "sugest": int(1 or 2)
	 *  }
	 */

	json request;
	request["query"] = query;
	request["offset"] = 0;
	request["suggest"] = 0;
	ws_window->send(request.dump());
}

void TradeWindow::Draw(IDirect3DDevice9* device) {
	if (visible) {
		if (ws_window == nullptr) {
			AsyncWindowConnect();
		}
	} else { // not visible
		if (ws_window) {
			ws_window->close();
			delete ws_window;
			ws_window = nullptr;
		}
		return;
	}
	
	if (ws_window && ws_window->getReadyState() == WebSocket::CLOSED) {
		delete ws_window;
		ws_window = nullptr;
	}

	// start the trade_searcher if its not active
	// if (!trade_searcher->is_active() && !trade_searcher->is_timed_out()) trade_searcher->search("");
	ImGui::SetNextWindowPosCenter(ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiSetCond_FirstUseEver);
	if (ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
		/* Search bar header */
		static char search_buffer[256];
		ImGui::PushItemWidth((ImGui::GetWindowContentRegionWidth() - 80.0f - 80.0f - 80.0f - ImGui::GetStyle().ItemInnerSpacing.x * 6));
		if (ImGui::InputText("", search_buffer, 256, ImGuiInputTextFlags_EnterReturnsTrue)) {
			search(search_buffer);
		}
		ImGui::SameLine();
		if (ImGui::Button("Search", ImVec2(80.0f, 0))) {
			search(search_buffer);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear", ImVec2(80.0f, 0))) {
			strncpy(search_buffer, "", 256);
			search("");
		}
		ImGui::SameLine();
		if (ImGui::Button("Alerts", ImVec2(80.0f, 0))) {
			show_alert_window = !show_alert_window;
		}

		/* Main trade chat area */
		ImGui::BeginChild("trade_scroll", ImVec2(0, -20.0f - ImGui::GetStyle().ItemInnerSpacing.y));
		/* Connection checks */
		if (!ws_window && !ws_window_connecting) {
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("The connection to kamadan.decltype.com has timed out.").x) / 2);
			ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2);
			ImGui::Text("The connection to kamadan.decltype.com has timed out.");
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Click to reconnect").x) / 2);
			if (ImGui::Button("Click to reconnect")) {
				AsyncWindowConnect();
			}
		} else if (ws_window_connecting || (ws_window && ws_window->getReadyState() == WebSocket::CONNECTING)) {
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Connecting...").x)/2);
			ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2);
			ImGui::Text("Connecting...");
		} else {
			/* Display trade messages */
			bool show_time = ImGui::GetWindowWidth() > 600.0f;

			char timetext[128];
			time_t now = time(nullptr);

			const float innerspacing = ImGui::GetStyle().ItemInnerSpacing.x;
			const float time_width = show_time ? 100.0f : 0.0f;
			const float playername_left = time_width + innerspacing; // player button left align
			const float playernamewidth = 160.0f;
			const float message_left = playername_left + playernamewidth + innerspacing;

			fetch();
			size_t size = messages.size();
			for (unsigned int i = size - 1; i < size; i--) {
				Message &msg = messages[i];
				ImGui::PushID(i);

				// ==== time elapsed column ====
				if (show_time) {
					// negative numbers have came from this before, it is probably just server client desync
					int time_since_message = (int)now - msg.timestamp;

					ImGui::PushFont(GuiUtils::GetFont(GuiUtils::FontSize::f16));
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.7f, .7f, .7f, 1.0f));

					// decide if days, hours, minutes, seconds...
					if ((int)(time_since_message / (60 * 60 * 24))) {
						int days = (int)(time_since_message / (60 * 60 * 24));
						_snprintf(timetext, 128, "%d %s ago", days, days > 1 ? "days" : "day");
					} else if ((int)(time_since_message / (60 * 60))) {
						int hours = (int)(time_since_message / (60 * 60));
						_snprintf(timetext, 128, "%d %s ago", hours, hours > 1 ? "hours" : "hour");
					} else if ((int)(time_since_message / (60))) {
						int minutes = (int)(time_since_message / 60);
						_snprintf(timetext, 128, "%d %s ago", minutes, minutes > 1 ? "minutes" : "minute");
					} else {
						_snprintf(timetext, 128, "%d %s ago", time_since_message, time_since_message > 1 ? "seconds" : "second");
					}
					ImGui::SetCursorPosX(playername_left - innerspacing - ImGui::CalcTextSize(timetext).x);
					ImGui::Text(timetext);
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}

				// ==== Sender name column ====
				if (show_time) {
					ImGui::SameLine(playername_left);
				}
				if (ImGui::Button(msg.name.c_str(), ImVec2(playernamewidth, 0))) {
					// open whisper to player
					GW::GameThread::Enqueue([&msg]() {
						wchar_t ws[100];
						swprintf(ws, 100, L"%hs", msg.name.c_str());
						GW::UI::SendUIMessage(GW::UI::kOpenWhisper, ws, nullptr);
					});
				}

				// ==== Message column ====
				ImGui::SameLine(message_left);
				ImGui::TextWrapped("%s", msg.message.c_str());
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		/* Link to website footer */
		if (ImGui::Button("Powered by https://kamadan.decltype.org", ImVec2(ImGui::GetWindowContentRegionWidth(), 20.0f))){ 
			CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			ShellExecute(NULL, "open", "https://kamadan.decltype.org", NULL, NULL, SW_SHOWNORMAL);
		}

		/* Alerts window */
		if (show_alert_window) {
			ImGui::SetNextWindowSize(ImVec2(250, 220), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Trade Alerts", &show_alert_window)) {
				DrawAlertsWindowContent(true);
			}
			ImGui::End();
		}
	}
	ImGui::End();
}

void TradeWindow::DrawAlertsWindowContent(bool ownwindow) {
	ImGui::Text("Alerts");
	ImGui::Checkbox("Send Kamadan ad1 trade chat to your trade chat", &print_game_chat);
	ImGui::Checkbox("Only show messages containing:", &filter_alerts);
	ImGui::TextDisabled("(Each line is a separate keyword. Not case sensitive.)");
	if (ImGui::InputTextMultiline("##alertfilter", alert_buf, ALERT_BUF_SIZE, 
		ImVec2(-1.0f, ownwindow ? -1.0f : 0.0f))) {

		alert_words = ParseBuffer(alert_buf);
		alertfile_dirty = true;
	}
}

void TradeWindow::DrawSettingInternal() {
	DrawAlertsWindowContent(false);
}

void TradeWindow::LoadSettings(CSimpleIni* ini) {
	ToolboxWindow::LoadSettings(ini);
	print_game_chat = ini->GetBoolValue(Name(), VAR_NAME(print_game_chat), false);
	filter_alerts   = ini->GetBoolValue(Name(), VAR_NAME(filter_alerts), false);

	std::ifstream alert_file;
	alert_file.open(Resources::GetPath(L"AlertKeywords.txt"));
	if (alert_file.is_open()) {
		alert_file.get(alert_buf, ALERT_BUF_SIZE, '\0');
		alert_file.close();
		alert_words = ParseBuffer(alert_buf);
	}
	alert_file.close();
}


void TradeWindow::SaveSettings(CSimpleIni* ini) {
	ToolboxWindow::SaveSettings(ini);

	ini->SetBoolValue(Name(), VAR_NAME(print_game_chat), print_game_chat);
	ini->SetBoolValue(Name(), VAR_NAME(filter_alerts), filter_alerts);

	if (alertfile_dirty) {
		std::ofstream bycontent_file;
		bycontent_file.open(Resources::GetPath(L"AlertKeywords.txt"));
		if (bycontent_file.is_open()) {
			bycontent_file.write(alert_buf, strlen(alert_buf));
			bycontent_file.close();
			alertfile_dirty = false;
		}
	}
}

std::vector<std::string> TradeWindow::ParseBuffer(const char *text) {
	std::vector<std::string> words;
	std::istringstream stream(text);
	std::string word;
	while (std::getline(stream, word)) {
		for (size_t i = 0; i < word.length(); i++)
			word[i] = tolower(word[i]);
		words.push_back(word);
	}
	return words;
}

std::vector<std::string> TradeWindow::ParseBuffer(std::fstream stream) {
	std::vector<std::string> words;
	std::string word;
	while (std::getline(stream, word)) {
		for (size_t i = 0; i < word.length(); i++)
			word[i] = tolower(word[i]);
		words.push_back(word);
	}
	return words;
}

void TradeWindow::AsyncChatConnect() {
	if (ws_chat) return;
	if (ws_chat_connecting) return;
	ws_chat_connecting = true;
	thread_jobs.push([this]() {
		if (!(ws_chat = WebSocket::from_url(ws_host))) {
			printf("Couldn't connect to the host '%s'", ws_host);
		}
		ws_chat_connecting = false;
	});
}

void TradeWindow::AsyncWindowConnect() {
	if (ws_window) return;
	if (ws_window_connecting) return;
	ws_window_connecting = true;
	thread_jobs.push([this]() {
		if (!(ws_window = WebSocket::from_url(ws_host))) {
			printf("Couldn't connect to the host '%s'", ws_host);
		}
		ws_window_connecting = false;
	});
}

void TradeWindow::Terminate() {
	should_stop = true;
	if (worker.joinable()) worker.join();
	if (ws_chat) delete ws_chat;
	if (ws_window) delete ws_window;
	WSACleanup();

	ToolboxWindow::Terminate();
}
