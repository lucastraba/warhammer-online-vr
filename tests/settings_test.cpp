#include "../src/settings.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace settings;
int main()
{
    std::string v;
    // WARVR.ini: comments and unknown keys survive, the last line wins, the value stops at the first space
    const std::string ini = "; my settings\r\ndraw_distance=1.3\r\nhud_width=2.0\r\n#laser=on\r\nlaser=on ; note\r\nlaser=off\r\n";
    CHECK(IniGet(ini, "laser", &v) && v == "off");
    CHECK(IniGet(ini, "LASER", &v) && v == "off");
    CHECK(!IniGet(ini, "auto_vr", &v));
    CHECK(IniGet(ini, "draw_distance", &v) && v == "1.3");
    {   const std::string out = IniSet(ini, "laser", "on");     // every laser line rewritten, the comment kept
        CHECK(out == "; my settings\r\ndraw_distance=1.3\r\nhud_width=2.0\r\n#laser=on\r\nlaser=on\r\nlaser=on\r\n");
        CHECK(IniGet(out, "laser", &v) && v == "on"); }
    {   const std::string out = IniSet(ini, "auto_vr", "on");   // appended, CRLF kept
        CHECK(out == ini + "auto_vr=on\r\n"); }
    CHECK(IniSet("", "log", "on") == "log=on\r\n");             // new file: Windows line endings
    CHECK(IniSet("a=1", "b", "2") == "a=1\nb=2\n");            // no newline at the end: added, LF kept
    CHECK(IniSet("a=1\n", "a", "3") == "a=3\n");
    CHECK(IniSet(" laser = off\r\n", "laser", "on") == "laser=on\r\n");   // the mod would not read " laser "; fixed
    CHECK(IniGet("server_dir=C:\\My Server\\bin\\Release\r\n", "server_dir", &v, true) && v == "C:\\My Server\\bin\\Release");
    CHECK(IniGet("server_dir=C:\\My Server\r\n", "server_dir", &v) && v == "C:\\My");
    {   const std::string out = IniDisable("hud_style=low\r\nhud_tilt=20\r\nhud_curve=on\r\n", "hud_tilt");
        CHECK(out == "hud_style=low\r\n; hud_tilt=20\r\nhud_curve=on\r\n");
        CHECK(!IniGet(out, "hud_tilt", &v)); }
    CHECK(IsOn("ON") && IsOn("1") && IsOn("yes") && !IsOn("off") && !IsOn(""));

    // dxvk.conf
    const std::string conf = "# mine\nd3d9.maxFrameRate = 60\nd3d9.samplerAnisotropy = 4\n";
    CHECK(ConfGet(conf, "d3d9.samplerAnisotropy", &v) && v == "4");
    CHECK(ConfSet(conf, "d3d9.samplerAnisotropy", "16") == "# mine\nd3d9.maxFrameRate = 60\nd3d9.samplerAnisotropy = 16\n");
    CHECK(ConfSet(conf, "d3d9.samplerAnisotropy", "") == "# mine\nd3d9.maxFrameRate = 60\n");
    CHECK(ConfSet("", "d3d9.samplerAnisotropy", "8") == "d3d9.samplerAnisotropy = 8\r\n");
    CHECK(ConfSet("", "d3d9.samplerAnisotropy", "") == "");

    // World.xml
    std::string xml = "<?xml version=\"1.0\"?>\r\n<WorldConfigs>\r\n  <RealmId>1</RealmId>\r\n  <GlobalLootRate>1</GlobalLootRate>\r\n</WorldConfigs>\r\n";
    CHECK(!XmlGet(xml, "TimeOfDayOffsetHours", &v));
    CHECK(XmlSet(&xml, "TimeOfDayOffsetHours", "12", "RealmId"));
    CHECK(xml == "<?xml version=\"1.0\"?>\r\n<WorldConfigs>\r\n  <RealmId>1</RealmId>\r\n  <TimeOfDayOffsetHours>12</TimeOfDayOffsetHours>\r\n  <GlobalLootRate>1</GlobalLootRate>\r\n</WorldConfigs>\r\n");
    CHECK(XmlGet(xml, "TimeOfDayOffsetHours", &v) && v == "12");
    CHECK(XmlSet(&xml, "TimeOfDayOffsetHours", "0", "RealmId"));
    CHECK(XmlGet(xml, "TimeOfDayOffsetHours", &v) && v == "0");
    CHECK(xml.find("<TimeOfDayOffsetHours>0</TimeOfDayOffsetHours>\r\n  <GlobalLootRate>") != std::string::npos);
    {   std::string x2 = "<WorldConfigs>\n\t<Other>2</Other>\n</WorldConfigs>\n";   // no RealmId: before the root's end
        CHECK(XmlSet(&x2, "TimeOfDayOffsetHours", "6", "RealmId"));
        CHECK(x2 == "<WorldConfigs>\n\t<Other>2</Other>\n  <TimeOfDayOffsetHours>6</TimeOfDayOffsetHours>\n</WorldConfigs>\n"); }
    {   std::string bad = "not xml";
        CHECK(!XmlSet(&bad, "TimeOfDayOffsetHours", "6", "RealmId")); }

    CHECK(DetectD3D9("") == D3D9Kind::None);
    CHECK(DetectD3D9(std::string("MZ\0\0dxvk-2.4", 12)) == D3D9Kind::Dxvk);
    CHECK(DetectD3D9("MZ..dgVoodoo D3D9") == D3D9Kind::DgVoodoo);
    CHECK(DetectD3D9("MZ..Microsoft") == D3D9Kind::Other);
    CHECK(ServerHasTimeOffset(std::string("..\0TimeOfDayOffsetHours\0..", 26)));
    CHECK(!ServerHasTimeOffset("..RealmId.."));

    std::printf(fails ? "settings tests FAILED (%d)\n" : "settings tests passed\n", fails);
    return fails ? 1 : 0;
}
