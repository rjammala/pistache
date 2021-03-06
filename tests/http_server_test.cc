#include <pistache/async.h>
#include <pistache/http.h>
#include <pistache/client.h>
#include <pistache/endpoint.h>

#include "gtest/gtest.h"

#include <chrono>
#include <future>
#include <fstream>
#include <string>

using namespace Pistache;

struct HelloHandlerWithDelay : public Http::Handler {
    HTTP_PROTOTYPE(HelloHandlerWithDelay)

    explicit HelloHandlerWithDelay(int delay = 0) : delay_(delay)
    { }

    void onRequest(const Http::Request& /*request*/, Http::ResponseWriter writer) override
    {
        std::this_thread::sleep_for(std::chrono::seconds(delay_));
        writer.send(Http::Code::Ok, "Hello, World!");
    }

    int delay_;
};

constexpr char SPECIAL_PAGE[] = "/specialpage";

struct SlowHandlerOnSpecialPage : public Http::Handler {
    HTTP_PROTOTYPE(SlowHandlerOnSpecialPage)

    explicit SlowHandlerOnSpecialPage(int delay = 0) : delay_(delay)
    { }

    void onRequest(const Http::Request& request, Http::ResponseWriter writer) override
    {
        if (request.resource() == SPECIAL_PAGE)
        {
            std::this_thread::sleep_for(std::chrono::seconds(delay_));
        }
        writer.send(Http::Code::Ok, "Hello, World!");
    }

    int delay_;
};

struct FileHandler : public Http::Handler
{
    HTTP_PROTOTYPE(FileHandler)

    explicit FileHandler(const std::string& fileName)
        : fileName_(fileName)
    { }

    void onRequest(const Http::Request& /*request*/, Http::ResponseWriter writer) override
    {
        Http::serveFile(writer, fileName_).then([this](ssize_t bytes)
        {
            std::cout << "Sent " << bytes << " bytes from " << fileName_ << " file" << std::endl;
        },
        Async::IgnoreException);
    }

private:
    std::string fileName_;
};

int clientLogicFunc(int response_size, const std::string& server_page, int wait_seconds)
{
    Http::Client client;
    client.init();

    std::vector<Async::Promise<Http::Response>> responses;
    auto rb = client.get(server_page);
    int counter = 0;
    for (int i = 0; i < response_size; ++i)
    {
        auto response = rb.send();
        response.then([&counter](Http::Response resp)
                      {
                          std::cout << "Response code is " << resp.code() << std::endl;
                          if (resp.code() == Http::Code::Ok)
                          {
                              ++counter;
                          }
                      },
                      Async::IgnoreException);
        responses.push_back(std::move(response));
    }

    auto sync = Async::whenAll(responses.begin(), responses.end());
    Async::Barrier<std::vector<Http::Response>> barrier(sync);
    barrier.wait_for(std::chrono::seconds(wait_seconds));

    client.shutdown();

    return counter;
}

TEST(http_server_test, client_disconnection_on_timeout_from_single_threaded_server) {
    const Pistache::Address address("localhost", Pistache::Port(0));

    Http::Endpoint server(address);
    auto flags = Tcp::Options::InstallSignalHandler | Tcp::Options::ReuseAddr;
    auto server_opts = Http::Endpoint::options().flags(flags);
    server.init(server_opts);
    const int SIX_SECONDS_DELAY = 6;
    server.setHandler(Http::make_handler<HelloHandlerWithDelay>(SIX_SECONDS_DELAY));
    server.serveThreaded();

    const std::string server_address = "localhost:" + server.getPort().toString();
    std::cout << "Server address: " << server_address << "\n";

    const int CLIENT_REQUEST_SIZE = 1;
    int counter = clientLogicFunc(CLIENT_REQUEST_SIZE, server_address, SIX_SECONDS_DELAY);

    server.shutdown();

    ASSERT_EQ(counter, 0);
}

TEST(http_server_test, client_multiple_requests_disconnection_on_timeout_from_single_threaded_server) {
    const Pistache::Address address("localhost", Pistache::Port(0));

    Http::Endpoint server(address);
    auto flags = Tcp::Options::InstallSignalHandler | Tcp::Options::ReuseAddr;
    auto server_opts = Http::Endpoint::options().flags(flags);
    server.init(server_opts);

    const int SIX_SECONDS_DELAY = 6;
    server.setHandler(Http::make_handler<HelloHandlerWithDelay>(SIX_SECONDS_DELAY));
    server.serveThreaded();

    const std::string server_address = "localhost:" + server.getPort().toString();
    std::cout << "Server address: " << server_address << "\n";

    const int CLIENT_REQUEST_SIZE = 3;
    int counter = clientLogicFunc(CLIENT_REQUEST_SIZE, server_address, SIX_SECONDS_DELAY);

    server.shutdown();

    ASSERT_EQ(counter, 0);
}

TEST(http_server_test, multiple_client_with_requests_to_multithreaded_server) {
    const Pistache::Address address("localhost", Pistache::Port(0));

    Http::Endpoint server(address);
    auto flags = Tcp::Options::InstallSignalHandler | Tcp::Options::ReuseAddr;
    auto server_opts = Http::Endpoint::options().flags(flags).threads(3);
    server.init(server_opts);
    server.setHandler(Http::make_handler<HelloHandlerWithDelay>());
    server.serveThreaded();

    const std::string server_address = "localhost:" + server.getPort().toString();
    std::cout << "Server address: " << server_address << "\n";

    const int SIX_SECONDS_TIMOUT = 6;
    const int FIRST_CLIENT_REQUEST_SIZE = 4;
    std::future<int> result1(std::async(clientLogicFunc,
                                        FIRST_CLIENT_REQUEST_SIZE,
                                        server_address,
                                        SIX_SECONDS_TIMOUT));
    const int SECOND_CLIENT_REQUEST_SIZE = 5;
    std::future<int> result2(std::async(clientLogicFunc,
                                        SECOND_CLIENT_REQUEST_SIZE,
                                        server_address,
                                        SIX_SECONDS_TIMOUT));

    int res1 = result1.get();
    int res2 = result2.get();

    server.shutdown();

    ASSERT_EQ(res1, FIRST_CLIENT_REQUEST_SIZE);
    ASSERT_EQ(res2, SECOND_CLIENT_REQUEST_SIZE);
}

TEST(http_server_test, multiple_client_with_different_requests_to_multithreaded_server) {
    const Pistache::Address address("localhost", Pistache::Port(0));

    Http::Endpoint server(address);
    auto flags = Tcp::Options::InstallSignalHandler | Tcp::Options::ReuseAddr;
    auto server_opts = Http::Endpoint::options().flags(flags).threads(3);
    server.init(server_opts);
    const int SIX_SECONDS_DELAY = 6;
    server.setHandler(Http::make_handler<SlowHandlerOnSpecialPage>(SIX_SECONDS_DELAY));
    server.serveThreaded();

    const std::string server_address = "localhost:" + server.getPort().toString();
    std::cout << "Server address: " << server_address << "\n";

    const int FIRST_CLIENT_REQUEST_SIZE = 1;
    std::future<int> result1(std::async(clientLogicFunc,
                                        FIRST_CLIENT_REQUEST_SIZE,
                                        server_address + SPECIAL_PAGE,
                                        SIX_SECONDS_DELAY / 2));
    const int SECOND_CLIENT_REQUEST_SIZE = 2;
    std::future<int> result2(std::async(clientLogicFunc,
                                        SECOND_CLIENT_REQUEST_SIZE,
                                        server_address,
                                        SIX_SECONDS_DELAY * 2));

    int res1 = result1.get();
    int res2 = result2.get();

    server.shutdown();

    if (hardware_concurrency() > 1)
    {
        ASSERT_EQ(res1, 0);
        ASSERT_EQ(res2, SECOND_CLIENT_REQUEST_SIZE);
    }
    else
    {
        ASSERT_TRUE(true);
    }
}

TEST(http_server_test, server_with_static_file)
{
    const std::string data("Hello, World!");
    char fileName[PATH_MAX] = "/tmp/pistacheioXXXXXX";
    mkstemp(fileName);
    std::cout << "Creating temporary file: " << fileName << '\n';

    std::ofstream tmpFile;
    tmpFile.open(fileName);
    tmpFile << data;
    tmpFile.close();

    const Pistache::Address address("localhost", Pistache::Port(0));

    Http::Endpoint server(address);
    auto flags = Tcp::Options::InstallSignalHandler | Tcp::Options::ReuseAddr;
    auto server_opts = Http::Endpoint::options().flags(flags);
    server.init(server_opts);
    server.setHandler(Http::make_handler<FileHandler>(fileName));
    server.serveThreaded();

    const std::string server_address = "localhost:" + server.getPort().toString();
    std::cout << "Server address: " << server_address << "\n";

    Http::Client client;
    client.init();
    auto rb = client.get(server_address);
    auto response = rb.send();
    std::string resultData;
    response.then([&resultData](Http::Response resp)
                  {
                      std::cout << "Response code is " << resp.code() << std::endl;
                      if (resp.code() == Http::Code::Ok)
                      {
                          resultData = resp.body();
                      }
                  },
                  Async::Throw);

    const int WAIT_TIME = 2;
    Async::Barrier<Http::Response> barrier(response);
    barrier.wait_for(std::chrono::seconds(WAIT_TIME));

    client.shutdown();
    server.shutdown();

    std::cout << "Deleting file " << fileName << std::endl;
    unlink(fileName);

    ASSERT_EQ(data, resultData);
}
