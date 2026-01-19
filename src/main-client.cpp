#include <sys/sysinfo.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/program_options.hpp>
#include <boost/system/detail/error_code.hpp>
#include <filesystem>
#include <iostream>

#include "remy/client.hpp"
#include "remy/logger.hpp"
#include "remy/view-client.hpp"

using namespace boost;

void print_help(const program_options::options_description &desc) {
  std::cerr << "Usage:" << std::endl;
  std::cerr << desc;
}

std::optional<int> parse_opts(int argc, const char *argv[], remy::Config &conf) {
  using namespace boost::program_options;
  options_description desc{"Remy client"};
  std::string dir;

  // clang-format off
  desc.add_options()
      ("help,h", "Help screen")
      ("addr,a", value<std::string>(&conf.addr)->default_value("127.0.0.1"), "Addr")
      ("port,p", value<uint16_t>(&conf.port)->default_value(8000), "Port")
      ("dir,C", value<std::string>(&dir), "Directory")
      ("log-file,o", value<std::string>(&conf.log_path), "Log file");
  // clang-format on

  parsed_options parsed_options = command_line_parser{argc, argv}.options(desc).allow_unregistered().run();

  variables_map vm;
  store(parsed_options, vm);
  notify(vm);

  if (vm.count("help")) {
    print_help(desc);
    return EXIT_SUCCESS;
  }
  if (vm.count("dir")) {
    std::cerr << std::format("INFO: change directory to '{}'\n", dir);
    std::filesystem::current_path(dir);
  }

  DBG(std::cerr << std::format("addr: {}\n", conf.addr);)
  DBG(std::cerr << std::format("port: {}\n", conf.port);)
  DBG(std::cerr << std::format("dir: {}\n", dir);)
  DBG(std::cerr << std::format("log file: {}\n", conf.log_path);)

  return {};
}

int main(int argc, const char *argv[]) {
  try {
    remy::Config conf;
    if (auto ret = parse_opts(argc, argv, conf); ret.has_value()) {
      return *ret;
    }

    std::shared_ptr<remy::FileLogWriter> file_log;
    if (!conf.log_path.empty()) {
      file_log = std::make_shared<remy::FileLogWriter>(conf.log_path);
      remy::Log.addObserver(file_log);
    }

    auto client_net = std::make_shared<remy::ClientNet>(conf);

    auto ui = std::make_shared<remy::ClientUi>(client_net);

    remy::Log.addObserver(ui);

    auto l = remy::Log.guard("Remy client");

    ui->run();

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "Unknown error" << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
