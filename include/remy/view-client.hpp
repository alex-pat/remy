#pragma once

#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <memory>

#include "remy/client.hpp"
#include "remy/common.hpp"
#include "remy/logger.hpp"

namespace remy {
using namespace std::chrono_literals;

class ClientNet;

class ClientUi final : public std::enable_shared_from_this<ClientUi>, public DistLog::Observer {
 public:
  ClientUi(std::shared_ptr<ClientNet> client);
  ~ClientUi() override;

  void run();

  // Functions called by ClientNet
  void update_clients(const ClientsListPayload &clients);
  void update_name(ClientName &&name);
  void show_dents(UiBrowserId id, PathDentsPayload &&path);
  void show_warning(std::string &&err_msg);
  void critical_error(std::string &&err_msg);
  void browser_error(UiBrowserId);
  void update_watcher_info(std::optional<WatcherInfo> &&info);
  void show_delete_result(std::string &&msg);

  void log(LogLevel, std::string_view) override;

 private:
  void create_panels();
  ftxui::Component create_main_container();
  ftxui::ComponentDecorator create_modal_help();
  ftxui::ComponentDecorator create_modal_new_name();
  ftxui::ComponentDecorator create_modal_warning();
  ftxui::ComponentDecorator create_modal_crit_err();
  ftxui::ComponentDecorator create_modal_copy();
  ftxui::ComponentDecorator create_modal_delete();

  /** Update clients list and current directories */
  void reload_info();

  std::shared_ptr<ClientNet> m_client;
  ftxui::ScreenInteractive m_screen;

  // Style of all buttons
  ftxui::ButtonOption BUTTON_OPTIONS;

  struct ClientsInfo {
    std::vector<ClientId> ids;
    std::vector<ClientName> names;
    std::vector<std::string> pretty_names; // Showed on screen
  };
  ClientsInfo m_clients;
  /** Showing client name in the bottom */
  ClientName m_pretty_name = "Client name is unknown";
  bool m_show_help = false;

  struct Panel {
    /** Panels are tabs showing clients list or connected browser */
    static constexpr int PANEL_CLIENTS = 0;
    static constexpr int PANEL_BROWSER = 1;
    static constexpr int PANEL_WAITING = 2;
    int m_view_selected = 0;

    /** Index of choosen client to connect in client list */
    int m_client_menu_index = 0;

    struct Browser {
      ClientId m_client_id;
      std::string m_name;
      std::filesystem::path m_cwd;
      std::string m_pretty_cwd;
      int m_menu_index = 0;
      std::vector<std::string> m_basenames = {".."};
      std::vector<FileMetainfo> m_metas;
      std::vector<bool> m_selected_dentries = {false}; // Track selected state of dentries

      /** Collect selected dentries from panel */
      RemoteDentries collect_selected_dentries() const;
    };
    Browser m_browser;
  };
  std::array<Panel, 2> m_panels;
  ftxui::Components m_panels_components;
  int m_split_size;  // For ResizableSplitLeft

  struct NewNameModal {
    std::string new_name;
    bool is_shown = false;
  };
  NewNameModal m_name_modal;

  struct ErrorModal {
    std::string message;
    bool is_shown = false;
  };
  ErrorModal m_warning_modal;
  ErrorModal m_critical_error_modal;

  struct LogsView {
    ftxui::Elements lines;
    bool is_shown = false;
  };
  LogsView m_logs;

  struct CopyModal {
    bool is_shown = false;

    bool direction = true;  // true is right-to-left

    /** CopyModal is hidden tab showing different steps of copying process */
    static constexpr int COPY_DIALOG = 0;
    static constexpr int COPY_PROGRESS = 1;
    static constexpr int COPY_COMPLETED = 2;
    static constexpr int COPY_FAILED = 3;
    int view_selected = 0;

    RemoteDentries src;
    RemoteDentry dst;

    WatcherInfo progress_info;
  };
  CopyModal m_copy_modal;
  void copy_dialog_payload();

  struct DeleteModal {
    bool is_shown = false;

    enum State {
      CONFIRMATION_DIALOG,
      WAITING,
      RESULT,
    } state = CONFIRMATION_DIALOG;

    UiBrowserId id;
    RemoteDentries entries;
    std::string result_msg;
  };
  DeleteModal m_delete_modal;
  void delete_dialog_payload();

  static constexpr std::chrono::duration DOUBLE_CLICK_TIME = 0.5s;
  std::chrono::time_point<std::chrono::steady_clock> m_last_click;
};

}  // namespace remy
