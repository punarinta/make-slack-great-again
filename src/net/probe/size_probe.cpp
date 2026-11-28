// Not a test: the smallest program that uses all of net (an HTTPS request and
// a wss:// socket), linked with a map so tools/size_report.py shows what the
// module and its TLS cost an app that links it.
#include "net/net.h"
#include "plat/plat.h"

int main(int argc, char **argv) {
    auto app = plat::App::create();
    if (!app || argc < 3)
        return 1;
    net::Client    client(*app);
    net::WebSocket ws(*app);
    ws.onText = [&](std::string) { app->quit(); };
    ws.open(argv[2]);
    client.send({.url = argv[1]}, [&](net::Response r) { ws.sendText(r.body); });
    app->run();
    return net::freeLoopbackPort() > 0 ? 0 : 1;
}
