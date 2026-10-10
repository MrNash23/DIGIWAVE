#include "ptt_if.h"

#include <LilyGoLib.h>

namespace {

class ExtensionPortPtt : public Ptt {
public:
    bool active_high = true;

    void begin() override
    {
        instance.io.pinMode(EXPANDS_GPIO_EN, OUTPUT);
        set(false);
    }

    void set(bool transmit) override { instance.io.digitalWrite(EXPANDS_GPIO_EN, transmit == active_high ? HIGH : LOW); }
};

ExtensionPortPtt ptt;

}  // namespace

Ptt &extensionPortPtt(bool active_high)
{
    ptt.active_high = active_high;
    return ptt;
}
