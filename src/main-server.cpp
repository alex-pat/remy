#include <sys/sysinfo.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <thread>

#include "remy/common.hpp"
#include "remy/logger.hpp"
#include "remy/server.hpp"

using namespace boost;

void print_help(const program_options::options_description &desc) {
  std::cerr << "Usage:" << std::endl;
  std::cerr << desc;
}

std::optional<int> parse_opts(int argc, const char *argv[], remy::Config &conf) {
  using namespace boost::program_options;
  options_description desc{"Remy server"};

  // clang-format off
  desc.add_options()
      ("help,h", "Help screen")
      ("addr,a", value<std::string>(&conf.addr)->default_value("127.0.0.1"), "Addr")
      ("port,p", value<uint16_t>(&conf.port)->default_value(8000), "Port")
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
  DBG(std::cout << std::format("addr: {}\n", conf.addr);)
  DBG(std::cout << std::format("port: {}\n", conf.port);)
  DBG(std::cout << std::format("log file: {}\n", conf.log_path);)

  return {};
}

int main(int argc, const char *argv[]) {
  try {
    remy::Config conf;
    if (auto ret = parse_opts(argc, argv, conf); ret.has_value()) {
      return *ret;
    }

    auto cout_log = std::make_shared<remy::CoutLogWriter>();
    remy::Log.addObserver(cout_log);

    std::shared_ptr<remy::FileLogWriter> file_log;
    if (!conf.log_path.empty()) {
      file_log = std::make_shared<remy::FileLogWriter>(conf.log_path);
      remy::Log.addObserver(file_log);
    }
    auto l = remy::Log.guard("Remy server");

    asio::io_context io_context;

    asio::signal_set signals(io_context, SIGINT, SIGTERM);

    signals.async_wait([&io_context](auto, auto) { io_context.stop(); });

    asio::co_spawn(io_context, remy::Server::run(conf, signals), asio::detached);

#if 0
    auto nproc = 1;
#else
    auto nproc = get_nprocs();
#endif
    std::vector<std::jthread> threads;
    for (auto i = 1; i < nproc; i++) {
      threads.emplace_back([&io_context, i]() {
        auto l = remy::Log.guard(std::format("Thread #{}", i));
        io_context.run();
      });
    }
    auto ll = remy::Log.guard("Thread #0");
    io_context.run();

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "Unknown error" << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
