#include "remy/view-client.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/flexbox_config.hpp>

#include "ftxui/component/component.hpp"
#include "remy/common.hpp"
#include "remy/logger.hpp"
#include "remy/utils.hpp"
namespace remy {

using namespace ftxui;

constexpr size_t LOG_BUF_LEN = 25;

ClientUi::ClientUi(std::shared_ptr<ClientNet> client)
    : m_client(client)
    , m_screen(ScreenInteractive::Fullscreen()) {
  BUTTON_OPTIONS = ButtonOption::Ascii();
  BUTTON_OPTIONS.animated_colors.foreground.Set(Color::Green, Color::GreenLight);
  BUTTON_OPTIONS.animated_colors.background.enabled = true;
}

ClientUi::~ClientUi() { m_client->stop(); }

namespace {
/** Helper function. Creates pretty colored name for dir/symlink/executable */
Element name_elem(const std::string &basename, uint16_t mode) {
  if (S_ISREG(mode) && (mode & S_IXUSR)) {
    return hbox({text(basename) | bold | color(Color::Green), text("*")});
  }
  if (S_ISDIR(mode)) {
    return hbox({text(basename) | bold | color(Color::Blue), text("/")});
  }
  if (S_ISLNK(mode)) {
    return hbox({text(basename) | bold | color(Color::Cyan), text("@")});
  }
  if (S_ISCHR(mode)) {
    return text(basename) | bold | color(Color::Yellow);
  }
  return text(basename);
}
/** Helper function. Creates right part of element in pane (size + mode) */
Element right_text(const FileMetainfo &meta) {
  std::stringstream ss;
  ss << utils::pretty_size(meta.size) << ' ';
  ss << (meta.mode & S_IRUSR ? 'r' : '-');
  ss << (meta.mode & S_IWUSR ? 'w' : '-');
  ss << (meta.mode & S_IXUSR ? 'x' : '-');
  ss << (meta.mode & S_IRGRP ? 'r' : '-');
  ss << (meta.mode & S_IWGRP ? 'w' : '-');
  ss << (meta.mode & S_IXGRP ? 'x' : '-');
  ss << (meta.mode & S_IROTH ? 'r' : '-');
  ss << (meta.mode & S_IWOTH ? 'w' : '-');
  ss << (meta.mode & S_IXOTH ? 'x' : '-');
  return text(ss.str());
}
}  // namespace

void ClientUi::run() {
  auto main_container = create_main_container();

  main_container |= create_modal_help();

  main_container |= create_modal_new_name();

  main_container |= create_modal_warning();

  main_container |= create_modal_crit_err();

  main_container |= create_modal_copy();

  // Run network thread and all logic
  m_client->run(weak_from_this());

  m_screen.Loop(main_container);
  Log.info("UI loop stopped");

  m_client->stop();
}

void ClientUi::create_panels() {
  m_panels_components.reserve(m_panels.size());
  UiBrowserId browser_id = 0;
  for (auto &pnl : m_panels) {
    // Clients menu
    auto clients_list = Menu({
        .entries = &m_clients.pretty_names,
        .selected = &pnl.m_client_menu_index,
        .on_enter =
            [this, &pnl, browser_id]() {
              auto client_id = m_clients.ids[pnl.m_client_menu_index];
              m_client->request_browser_connection(browser_id, client_id);
              pnl.m_browser.m_client_id = client_id;
              pnl.m_browser.m_name = m_clients.names[pnl.m_client_menu_index];
              pnl.m_view_selected = Panel::PANEL_WAITING;
            },
    });
    clients_list |= Renderer([](Element inner) {
      return vbox({
          text("Choose client to connect") | hcenter | bold | color(Color::Blue),
          inner | vscroll_indicator | yframe,
          filler(),
      });
    });
    // Connected Browser
    auto browser = Menu({
        .entries = &pnl.m_browser.m_basenames,
        .selected = &pnl.m_browser.m_menu_index,
        .entries_option =
            {
                .transform =  // Renders pretty line for dentry
                [&pnl](const EntryState &state) {
                  Elements elems;
                  elems.push_back(text(state.active ? ">" : " "));
                  elems.push_back(
                    pnl.m_browser.m_selected_dentries[state.index] ?
                      (text("*") | color(Color::Yellow) | bold)
                      : text(" "));
                  if (state.index == 0) {
                    if (pnl.m_browser.m_cwd.empty()) {
                      elems.push_back(text(".. (Disconnect and show clients list)") | italic);
                    } else {
                      elems.push_back(text(state.label));
                    }
                  } else {
                    const auto &meta = pnl.m_browser.m_metas[state.index];
                    elems.push_back(name_elem(state.label, meta.mode));
                    elems.push_back(filler());

                    elems.push_back(right_text(meta));
                  }
                  auto box = hbox(elems);
                  if (state.focused) {
                    box |= inverted;
                  }
                  if (state.active) {
                    box |= bold;
                  }
                  return box;
                },
            },
        .on_enter =
            [this, &pnl, browser_id]() {
              if (pnl.m_browser.m_menu_index == 0) {
                // parent or home
                if (pnl.m_browser.m_cwd.empty()) {
                  m_client->disconnect_browser(browser_id);

                  pnl.m_view_selected = Panel::PANEL_CLIENTS;
                  return;
                }
                pnl.m_browser.m_cwd = pnl.m_browser.m_cwd.parent_path();
              } else {
                if (!S_ISDIR(pnl.m_browser.m_metas[pnl.m_browser.m_menu_index].mode)) {
                  return;
                }
                pnl.m_browser.m_cwd /= pnl.m_browser.m_basenames[pnl.m_browser.m_menu_index];
              }
              m_client->cd(browser_id, pnl.m_browser.m_cwd);
              pnl.m_view_selected = Panel::PANEL_WAITING;
            },
    }) | CatchEvent([this, &pnl](Event event) { // Handle Space / right clicks for selection
        if (event == Event::Character(' ') && pnl.m_browser.m_menu_index > 0) {
          pnl.m_browser.m_selected_dentries[pnl.m_browser.m_menu_index] = !pnl.m_browser.m_selected_dentries[pnl.m_browser.m_menu_index];
          pnl.m_browser.m_menu_index =
            std::min<int>(pnl.m_browser.m_menu_index + 1,
                          pnl.m_browser.m_basenames.size() - 1);
          return true;
        }
        if (event.is_mouse()) {
          auto ms = event.mouse();
          if (ms.button == Mouse::Right && ms.motion == Mouse::Pressed) {
            ms.button = Mouse::Left;
            ms.y++; // for some reason ???
            m_screen.PostEvent(Event::Mouse("", ms));
            m_screen.PostEvent(Event::Character(' '));
            m_screen.PostEvent(Event::Character('k')); // up, compensate increasing did by selection
            return true;
          }
        }
        return false;
    });
    browser |= Renderer([&pnl](Element inner) {
      return vbox({
          text(pnl.m_browser.m_pretty_cwd) | bold | color(Color::Green),
          inner | vscroll_indicator | yframe,
          filler(),
      });
    });
    // Waiting tab (while connecting to browser)
    auto waiting = Renderer([] { return text("Waiting...") | center; });

    auto tbcntrl = Container::Tab(
        {
            clients_list,
            browser,
            waiting,
        },
        &pnl.m_view_selected);
    m_panels_components.push_back(tbcntrl);
    browser_id++;
  }
}

Component ClientUi::create_main_container() {
  create_panels();

  m_split_size = Terminal::Size().dimx / 2;
  auto wins_split = ResizableSplitLeft(m_panels_components[0], m_panels_components[1], &m_split_size);

  auto indicator_str = std::format("Server: {}:{}", m_client->m_conf.addr, m_client->m_conf.port);
  auto connected_indicator = Renderer([indicator_str] { return text(indicator_str); });
  auto status_indicator = Renderer([this] {
    Elements elems = {text(m_pretty_name)};
    if (auto n_browsers = m_client->browser_hosts_count(); n_browsers) {
      elems.push_back(filler());
      elems.push_back(text(std::format("Clients attached: {}", n_browsers)));
    }
    return hbox(elems);
  });

  //// Buttons

  auto help_button = Button(
      "[F1] Show help", [this] { m_show_help = true; }, BUTTON_OPTIONS);
  auto name_button = Button(
      "Set client [n]ame", [this] { m_name_modal.is_shown = true; }, BUTTON_OPTIONS);
  auto copy_button = Button(
      "[c]opy", [this] { copy_dialog_payload(); }, BUTTON_OPTIONS);
  auto clients_button = Button(
      "[r]eload", [this] { reload_info(); }, BUTTON_OPTIONS);
  auto logs_button = Button(
      "Show lo[g]s", [this] { m_logs.is_shown = !m_logs.is_shown; }, BUTTON_OPTIONS);
  auto quit_button = Button(
      "[q]uit", [this] { m_screen.Exit(); }, BUTTON_OPTIONS);
  auto logs_view = Renderer([this] {
                     return vbox({
                                separator(),
                                filler(),
                                vbox(m_logs.lines) | focusPositionRelative(0.f, 1.f) | yframe,
                            }) |
                            size(HEIGHT, EQUAL, std::min<int>(LOG_BUF_LEN + 1, Terminal::Size().dimy / 3));
                   }) |
                   Maybe(&m_logs.is_shown);

  auto main_container = Container::Vertical({
      connected_indicator,
      wins_split,
      status_indicator,
      Container::Horizontal({
          help_button,
          name_button,
          copy_button,
          clients_button,
          logs_button,
          quit_button,
      }),
      logs_view,
  });

  main_container |= Renderer([=](Element inner) {
    return window(text(" remy ") | hcenter | size(WIDTH, EQUAL, Terminal::Size().dimx),
                  vbox({
                      connected_indicator->Render(),
                      separator(),
                      wins_split->Render() | yflex,
                      separator(),
                      status_indicator->Render(),
                      separator(),
                      flexbox(
                          {
                              help_button->Render(),
                              name_button->Render(),
                              copy_button->Render(),
                              clients_button->Render(),
                              logs_button->Render(),
                              quit_button->Render(),
                          },
                          FlexboxConfig{.justify_content = FlexboxConfig::JustifyContent::SpaceEvenly}) |
                          xflex,
                      logs_view->Render(),
                  }));
  });

  // Key/Mouse events
  main_container |= CatchEvent([this](Event event) {
    if (event == Event::F1) {
      m_show_help = true;
      return true;
    }
    if (event == Event::Character('r')) {
      reload_info();
      return true;
    }
    if (event == Event::Character('c')) {
      copy_dialog_payload();
      return true;
    }
    if (event == Event::Character('n')) {
      m_name_modal.is_shown = true;
      return true;
    }
    if (event == Event::Character('g')) {
      m_logs.is_shown = !m_logs.is_shown;
      return true;
    }
    if (event == Event::Character('q')) {
      m_screen.Exit();
      return true;
    }
    if (event.is_mouse()) {
      auto ms = event.mouse();
      if (ms.button == Mouse::Left && ms.motion == Mouse::Released) {
        auto cur_click = std::chrono::steady_clock::now();

        auto is_double_clicked = (cur_click - m_last_click) < DOUBLE_CLICK_TIME;
        m_last_click = cur_click;

        if (is_double_clicked) {
          m_screen.PostEvent(Event::Return);
          return true;
        }
      }
    }
    return false;
  });
  return main_container;
}

ComponentDecorator ClientUi::create_modal_help() {
  auto help = gridbox({
      {text("hjkl / Arrows") | align_right, separatorEmpty(), text("Browsing (mouse scroll also works)")},
      {text("Enter / Double click") | align_right, separatorEmpty(), text("Enter directory / client")},
      {text("r") | align_right, separatorEmpty(), text("Force reload clients list or dir")},
      {text("c") | align_right, separatorEmpty(), text("Copy")},
      {text("n") | align_right, separatorEmpty(), text("Set new name for this client")},
      {text("g") | align_right, separatorEmpty(), text("Toggle showing logs")},
      {text("F1") | align_right, separatorEmpty(), text("Show this help")},
      {text("q / Ctrl-c") | align_right, separatorEmpty(), text("Quit")},
  });
  auto win = Button(
      "Close", [this] { m_show_help = false; }, BUTTON_OPTIONS);
  win |= Renderer([help](Element inner) {
    return window(text("Help"), vbox({
                                    help,
                                    text("Note: panels are resizable with mouse"),
                                    inner,
                                }));
  });
  win |= CatchEvent([this](Event event) {
    if (event == Event::F1 || event == Event::Character('q')) {
      m_show_help = false;
      return true;
    }
    return false;
  });

  return Modal(win, &m_show_help);
}

ComponentDecorator ClientUi::create_modal_new_name() {
  auto apply_new_name = [this] {
    utils::trim_end(m_name_modal.new_name);
    m_client->set_new_name(m_name_modal.new_name);
    m_name_modal.is_shown = false;
    m_pretty_name = "Setting new name...";
  };
  auto name_modal = Container::Vertical({
      Input(InputOption{
          .content = &m_name_modal.new_name,
          .placeholder = "Enter new name here",
          .on_enter = apply_new_name,
      }),
      Container::Horizontal({
          Button("Ok", apply_new_name, BUTTON_OPTIONS),
          Button(
              "Cancel", [this] { m_name_modal.is_shown = false; }, BUTTON_OPTIONS),
      }),
  });
  name_modal |= Renderer(
      [](Element inner) { return window(text("Set new name") | bold, inner) | size(WIDTH, GREATER_THAN, 30); });
  return Modal(name_modal, &m_name_modal.is_shown);
}

ComponentDecorator ClientUi::create_modal_warning() {
  auto warning = Button(
      "Ok", [this] { m_warning_modal.is_shown = false; }, BUTTON_OPTIONS);
  warning |= Renderer([this](Element inner) {
    return window(text("Warning") | bold | color(Color::Red),
                  vbox({paragraph(m_warning_modal.message), separator(), hbox({inner, filler()})})) |
           size(WIDTH, GREATER_THAN, 70);
  });
  return Modal(warning, &m_warning_modal.is_shown);
}

ComponentDecorator ClientUi::create_modal_crit_err() {
  auto critical_error_modal = Button(
      "Exit", [this] { m_screen.Exit(); }, BUTTON_OPTIONS);
  critical_error_modal |= Renderer([this](Element inner) {
    return window(text("CRITICAL ERROR") | bold | color(Color::Red),
                  vbox({paragraph(m_critical_error_modal.message), separator(), hbox({inner, filler()})})) |
           size(WIDTH, GREATER_THAN, 70);
  });
  return Modal(critical_error_modal, &m_critical_error_modal.is_shown);
}

ComponentDecorator ClientUi::create_modal_copy() {
  auto copy_modal = Container::Tab(
      {
          // Dialog of choosing direction and confirming
          Container::Horizontal({
              Button(
                  "Cancel", [this] { m_copy_modal.is_shown = false; }, BUTTON_OPTIONS),
              Button(
                  "Change direction", [this] { copy_dialog_payload(); }, BUTTON_OPTIONS),
              Button(
                  "Proceed",
                  [this] {
                    m_client->request_copy(m_copy_modal.src, m_copy_modal.dst);
                    m_copy_modal.progress_info = {.files_all = 1};
                    m_copy_modal.view_selected = CopyModal::COPY_PROGRESS;
                  },
                  BUTTON_OPTIONS),
          }) | Renderer([this](Element inner) {
            auto make_text = [this](bool is_left) {
              std::stringstream out;
              if ((is_left && !m_copy_modal.direction) || (!is_left && m_copy_modal.direction)) {
                out << m_copy_modal.src.dentries.size()
                    << " in dir: '"
                    << m_copy_modal.src.basedir << "':\n";
                std::copy(m_copy_modal.src.dentries.cbegin(), m_copy_modal.src.dentries.cend(),
                          std::ostream_iterator<std::string>(out, "\n"));
              } else {
                out << m_copy_modal.dst.second;
              }
              return out.str();
            };
            std::string left_text = make_text(true);
            std::string right_text = make_text(false);
            auto direction = m_copy_modal.direction ? " <- " : " -> ";
            return vbox({
                       hbox({
                           vbox({
                               text(m_panels[0].m_browser.m_name) | align_right,
                               paragraph(left_text),
                           }),
                           text(direction) | bold,
                           vbox({
                               text(m_panels[1].m_browser.m_name),
                               paragraph(right_text),
                           }),
                       }),
                       inner,
                   }) |
                   xflex;
          }),
          // Copy progress
          Renderer([this]() {
            const auto &progress = m_copy_modal.progress_info;
            float overall = static_cast<float>(progress.files_completed) / progress.files_all;
            float cur_file_progress =
                static_cast<float>(progress.cur_progress) / (progress.cur_size ? progress.cur_size : 1);
            return vbox({
                gridbox({
                    {
                        text("Overall progress: "),
                        separator(),
                        gauge(overall),
                        separator(),
                        text(std::format("{}/{}", progress.files_completed, progress.files_all)),
                    },
                    {
                        text("Current file"),
                        separator(),
                        gauge(cur_file_progress),
                        separator(),
                        text(std::format("{}/{}", utils::pretty_size(progress.cur_progress),
                                         utils::pretty_size(progress.cur_size))),
                    },
                }),
                text(progress.cur_file),
            });
          }),
          // Success view
          Button(
              "Ok",
              [this] {
                m_copy_modal.is_shown = false;
                m_copy_modal.view_selected = CopyModal::COPY_DIALOG;
                reload_info();
              },
              BUTTON_OPTIONS) |
              Renderer([](Element inner) {
                return vbox({text("Completed!"), separator(), inner});
              }),
          // Fail view
          Button(
              "Ok",
              [this] {
                m_copy_modal.is_shown = false;
                m_copy_modal.view_selected = CopyModal::COPY_DIALOG;
              },
              BUTTON_OPTIONS) |
              Renderer([](Element inner) {
                return vbox({text("Copy failed!"), separator(), inner});
              }),
      },
      &m_copy_modal.view_selected);
  copy_modal |= Renderer([](Element inner) { return window(text(" Copy "), inner) | size(WIDTH, GREATER_THAN, 70); });

  return Modal(copy_modal, &m_copy_modal.is_shown);
}

void ClientUi::reload_info() {
  m_client->request_clients_list();
  // Reload current dirs
  for (UiBrowserId id = 0; id < m_panels.size(); id++) {
    if (m_panels[id].m_view_selected == Panel::PANEL_BROWSER) {
      // We have a client connected in this panel, update directory by cd to same dir
      m_client->cd(id, m_panels[id].m_browser.m_cwd);
    }
  }
}

/**
 * Public functions called by ClientNet
 * Note: lambdas passed to `m_screen.Post` are called inside main UI loop so it doesn't need any locks
 */

void ClientUi::update_clients(const ClientsListPayload &clients) {
  m_screen.Post([this, clients] {
    Log.info("Got clients list");
    m_clients.ids.clear();
    m_clients.names.clear();
    m_clients.pretty_names.clear();
    for (auto [id, name] : clients) {
      m_clients.ids.push_back(id);
      m_clients.names.push_back(name);
      m_clients.pretty_names.push_back(std::format("{} ({})", name, id));
    }
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::update_name(ClientName &&name) {
  m_screen.Post([this, new_name = std::move(name)] {
    Log.info("Name updated");
    m_pretty_name = std::format("Client name: {}", new_name);
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::show_warning(std::string &&err_msg) {
  m_screen.Post([this, msg = std::move(err_msg)]() mutable {
    m_warning_modal.message = std::move(msg);
    m_warning_modal.is_shown = true;
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::critical_error(std::string &&err_msg) {
  m_screen.Post([this, msg = std::move(err_msg)]() mutable {
    m_critical_error_modal.message = std::move(msg);
    m_critical_error_modal.is_shown = true;
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::browser_error(UiBrowserId id) {
  m_screen.Post([this, id] {
    auto &panel = m_panels[id];
    m_warning_modal.message = std::format("Client {} unexpectedly disconnected", panel.m_browser.m_name);
    m_warning_modal.is_shown = true;
    panel.m_view_selected = Panel::PANEL_CLIENTS;
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::show_dents(UiBrowserId id, PathDentsPayload &&payload) {
  m_screen.Post([this, id, payload = std::move(payload)]() mutable {
    auto &panel = m_panels[id];
    auto &browser = panel.m_browser;
    auto [path, dents_] = std::move(payload);

    panel.m_view_selected = Panel::PANEL_BROWSER;
    if (!dents_.has_value()) {
      auto msg = Log.warn("cd to '{}' failed", path);
      m_warning_modal.message = std::move(msg);
      m_warning_modal.is_shown = true;

      return;
    }
    Log.info("UI: updating dentries");
    auto dents = std::move(*dents_);

    browser.m_pretty_cwd = browser.m_name + ":" + path;
    browser.m_cwd = std::move(path);

    browser.m_menu_index = 0;
    browser.m_basenames.resize(1);
    browser.m_metas.resize(1);
    browser.m_selected_dentries.assign(dents.size() + 1, false);
    std::sort(dents.begin(), dents.end(), [](const Dentry &first, const Dentry &second) mutable {
      if ((first.metainfo.mode & S_IFMT) != (second.metainfo.mode & S_IFMT)) {
        if (S_ISDIR(first.metainfo.mode)) {
          return true;
        }
        if (S_ISDIR(second.metainfo.mode)) {
          return false;
        }
      }
      return (first.basename <=> second.basename) <= 0;
    });
    for (auto &[name, meta] : dents) {
      browser.m_basenames.emplace_back(std::move(name));
      browser.m_metas.emplace_back(std::move(meta));
    }
  });
  m_screen.Post(Event::Custom);
}

/** Calculates `RemoteDentries` for copy. To change direction, call it again */
void ClientUi::copy_dialog_payload() {
  if (m_panels[0].m_view_selected != Panel::PANEL_BROWSER || m_panels[1].m_view_selected != Panel::PANEL_BROWSER) {
    Log.warn("Two clients must be opened to be able to copy");
    return;
  }

  auto &left = m_panels[0].m_browser;
  auto &right = m_panels[1].m_browser;

  if (left.m_basenames.size() <= 1 && right.m_basenames.size() <= 1) {
    Log.warn("Both are empty, can't choose what to copy");
    return;
  }

  if (!m_copy_modal.is_shown) {
    // We just opened the dialog, use focused panel as source
    m_copy_modal.direction = m_panels_components[1]->Focused();
  } else if (left.m_basenames.size() <= 1) {
    Log.warn("Left is empty, can only be destination");
    m_copy_modal.direction = true;
  } else if (right.m_basenames.size() <= 1) {
    Log.warn("Right is empty, can only be destination");
    m_copy_modal.direction = false;
  } else {
    // If calling it again, switch the direction
    m_copy_modal.direction = !m_copy_modal.direction;
  }

  // Determine source and destination browsers
  Panel::Browser *src_browser;
  Panel::Browser *dst_browser;

  if (!m_copy_modal.direction) {  // Left to right
    src_browser = &left;
    dst_browser = &right;
  } else {  // Right to left
    src_browser = &right;
    dst_browser = &left;
  }

  DBG(Log.trace("src basenames size {}", src_browser->m_basenames.size());)
  DBG(Log.trace("dst basenames size {}", dst_browser->m_basenames.size());)
  DBG(Log.trace("selected dentries size {}", src_browser->m_selected_dentries.size());)

  // Construct RemoteSrc and RemoteDest
  m_copy_modal.src = {src_browser->m_client_id, src_browser->m_cwd.string(), {}};
  m_copy_modal.dst = {dst_browser->m_client_id, dst_browser->m_cwd.string()};

  // Collect explicitly selected files (starting from index 1 to skip "..")
  for (size_t i = 1; i < src_browser->m_selected_dentries.size(); ++i) {
    if (src_browser->m_selected_dentries[i]) {
      DBG(Log.trace("Adding {}", src_browser->m_basenames[i]);)
      m_copy_modal.src.dentries.push_back(src_browser->m_basenames[i]);
    }
  }

  DBG(Log.trace("copied from selected {}", m_copy_modal.src.dentries.size());)

  if (m_copy_modal.src.dentries.empty()) {
    // No explicit multiple selections, fallback to single item or current directory.
    if (src_browser->m_menu_index == 0) {
      DBG(Log.trace("copying all src dentries");)
      // If ".." is highlighted and nothing else is selected, copy current directory.
      m_copy_modal.src.dentries.assign(
        std::next(src_browser->m_basenames.cbegin()),
        src_browser->m_basenames.cend());
    } else {
      DBG(Log.trace("copying a single dentry");)
      // Otherwise, copy the single highlighted item.
      m_copy_modal.src.dentries.push_back(src_browser->m_basenames[src_browser->m_menu_index]);
    }
  }

  if (m_copy_modal.src.dentries.empty()) [[unlikely]] {
    Log.err("No entries to send");
    return;
  }
  m_copy_modal.is_shown = true;
}

void ClientUi::update_watcher_info(std::optional<WatcherInfo> &&info) {
  m_screen.Post([this, new_info = std::move(info)]() mutable {
    if (new_info.has_value()) {
      auto &info = m_copy_modal.progress_info;
      info = std::move(*new_info);
      if (info.files_completed != info.files_all) {
        m_copy_modal.view_selected = CopyModal::COPY_PROGRESS;
      } else {
        m_copy_modal.view_selected = CopyModal::COPY_COMPLETED;
      }
    } else {
      Log.err("Copying process failed");
      m_copy_modal.view_selected = CopyModal::COPY_FAILED;
    }
  });
  m_screen.Post(Event::Custom);
}

void ClientUi::log(LogLevel level, std::string_view line) {
  auto elem = paragraph(std::string(line));
  switch (level) {
    using enum LogLevel;
    case Info:
      elem |= color(Color::Green);
      break;
    case Warning:
      elem |= color(Color::Yellow);
      break;
    case Critical:
      elem |= bold;
      [[fallthrough]];
    case Error:
      elem |= color(Color::Red);
      break;
    default:
      break;
  }
  m_screen.Post([this, elem] {
    if (m_logs.lines.size() >= LOG_BUF_LEN) {
      m_logs.lines.erase(m_logs.lines.begin());
    }
    m_logs.lines.push_back(elem);
  });
  m_screen.Post(Event::Custom);
}

}  // namespace remy
