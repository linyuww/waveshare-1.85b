#include "xiaozhi_protocol.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    using xiaozhi_protocol::ChatAction;
    assert(xiaozhi_protocol::toggleChatAction(true, false, false) == ChatAction::Ignore);
    assert(xiaozhi_protocol::toggleChatAction(false, false, false) == ChatAction::Connect);
    assert(xiaozhi_protocol::toggleChatAction(false, true, true) == ChatAction::Stop);
    assert(xiaozhi_protocol::toggleChatAction(false, true, false) == ChatAction::Listen);
    char device_id[18];
    assert(xiaozhi_protocol::deviceId("28:84:85:B2:1C:2C", device_id));
    assert(strcmp(device_id, "28:84:85:b2:1c:2c") == 0);
    assert(xiaozhi_protocol::deviceId("28:84:85:b2:1c:2c", device_id));
    assert(strcmp(device_id, "28:84:85:b2:1c:2c") == 0);
    assert(xiaozhi_protocol::deviceId("00:01:0A:aB:fE:FF", device_id));
    assert(strcmp(device_id, "00:01:0a:ab:fe:ff") == 0);
    const char *invalid_macs[] = {nullptr, "", "28:84:85:B2:1C:2", "28-84-85-B2-1C-2C", "28:84:85:B2:1C:2G", "28:84:85:B2:1C:2C\r\n"};
    for (const char *invalid : invalid_macs) {
        assert(!xiaozhi_protocol::deviceId(invalid, device_id));
        assert(device_id[0] == 0);
    }
    puts("PASS: original-style chat toggle, lowercase identity and invalid MAC/header injection rejection");
}
