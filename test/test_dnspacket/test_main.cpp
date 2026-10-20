// Тесты lib/dnspacket: ответы captive DNS и mDNS на собранные вручную запросы.
#include <string.h>
#include <unity.h>

#include <DnsPacket.h>

#include "../helpers/TestHelpers.h"

using namespace dnspacket;

void setUp() {}
void tearDown() {}

static const uint8_t kIp[4] = {192, 168, 4, 1};

// Запрос: заголовок + вопросы с именами из меток через точку.
static size_t buildQuery(uint8_t* out, uint16_t flags, const char* const* names, size_t count,
    uint16_t type, uint16_t qclass) {
    memset(out, 0, 12);
    out[0] = 0x12;
    out[1] = 0x34;
    out[2] = static_cast<uint8_t>(flags >> 8);
    out[3] = static_cast<uint8_t>(flags);
    out[5] = static_cast<uint8_t>(count);
    size_t at = 12;
    for (size_t q = 0; q < count; ++q) {
        const char* name = names[q];
        while (*name != '\0') {
            const char* dot = strchr(name, '.');
            size_t length = dot != nullptr ? static_cast<size_t>(dot - name) : strlen(name);
            out[at++] = static_cast<uint8_t>(length);
            memcpy(out + at, name, length);
            at += length;
            name += length + (dot != nullptr ? 1 : 0);
        }
        out[at++] = 0;
        out[at++] = static_cast<uint8_t>(type >> 8);
        out[at++] = static_cast<uint8_t>(type);
        out[at++] = static_cast<uint8_t>(qclass >> 8);
        out[at++] = static_cast<uint8_t>(qclass);
    }
    return at;
}

static void test_captive_answers_any_name() {
    uint8_t packet[kMaxPacket];
    const char* names[] = {"example.com"};
    size_t length = buildQuery(packet, 0x0100, names, 1, 1, 1);
    size_t answer = buildCaptiveDnsAnswer(packet, static_cast<int>(length), sizeof(packet), kIp);
    TEST_ASSERT_EQUAL_size_t(length + 16, answer);
    TEST_ASSERT_EQUAL_HEX8(0x12, packet[0]);   // ID сохранен
    TEST_ASSERT_EQUAL_HEX8(0x81, packet[2]);
    TEST_ASSERT_EQUAL_HEX8(0x80, packet[3]);
    TEST_ASSERT_EQUAL_HEX8(1, packet[7]);      // ANCOUNT = 1
    const uint8_t expected[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0x3C, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, packet + length, 16);
}

static void test_captive_ignores_bad_packets() {
    uint8_t packet[kMaxPacket];
    const char* names[] = {"example.com"};
    size_t length = buildQuery(packet, 0x8180, names, 1, 1, 1);  // это ответ, а не запрос
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, static_cast<int>(length), sizeof(packet), kIp));
    length = buildQuery(packet, 0x0100, names, 1, 1, 1);
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, 16, sizeof(packet), kIp));
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, -1, sizeof(packet), kIp));
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, static_cast<int>(length) - 2, sizeof(packet), kIp));
    packet[12] = 0xC0;  // указатель сжатия в вопросе не поддерживается
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, static_cast<int>(length), sizeof(packet), kIp));
    // Ответ не помещается в буфер
    length = buildQuery(packet, 0x0100, names, 1, 1, 1);
    TEST_ASSERT_EQUAL_size_t(0, buildCaptiveDnsAnswer(packet, static_cast<int>(length), length + 15, kIp));
}

static MdnsReply mdns(const char* name, uint16_t type, uint16_t qclass, uint16_t port,
    uint8_t* out, size_t& queryLength) {
    static uint8_t packet[kMaxPacket];
    const char* names[] = {name};
    queryLength = buildQuery(packet, 0x0000, names, 1, type, qclass);
    return buildMdnsAnswer(packet, static_cast<int>(queryLength), port, "opta-settings", kIp,
        out, kMaxPacket);
}

static void test_mdns_answers_own_name() {
    uint8_t out[kMaxPacket];
    size_t queryLength = 0;
    MdnsReply reply = mdns("opta-settings.local", 1, 1, 5353, out, queryLength);
    ASSERT_ENUM(MdnsStatus::Answer, reply.status);
    TEST_ASSERT_FALSE(reply.unicast);
    TEST_ASSERT_EQUAL_size_t(queryLength + 16, reply.length);
    TEST_ASSERT_EQUAL_HEX8(0x84, out[2]);
    TEST_ASSERT_EQUAL_HEX8(1, out[7]);
    const uint8_t expected[16] = {0xC0, 12, 0, 1, 0x80, 1, 0, 0, 0, 0x78, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out + queryLength, 16);
}

static void test_mdns_name_case_and_types() {
    uint8_t out[kMaxPacket];
    size_t queryLength = 0;
    ASSERT_ENUM(MdnsStatus::Answer, mdns("OPTA-Settings.LOCAL", 1, 1, 5353, out, queryLength).status);
    ASSERT_ENUM(MdnsStatus::Answer, mdns("opta-settings.local", 255, 1, 5353, out, queryLength).status);
    ASSERT_ENUM(MdnsStatus::NotOurName, mdns("opta-settings.local", 28, 1, 5353, out, queryLength).status);
    ASSERT_ENUM(MdnsStatus::NotOurName, mdns("other.local", 1, 1, 5353, out, queryLength).status);
    ASSERT_ENUM(MdnsStatus::NotOurName, mdns("opta-settings.local.x", 1, 1, 5353, out, queryLength).status);
}

static void test_mdns_unicast() {
    uint8_t out[kMaxPacket];
    size_t queryLength = 0;
    TEST_ASSERT_TRUE(mdns("opta-settings.local", 1, 1, 50000, out, queryLength).unicast);
    TEST_ASSERT_TRUE(mdns("opta-settings.local", 1, 0x8001, 5353, out, queryLength).unicast);  // бит QU
}

static void test_mdns_second_question_matches() {
    uint8_t packet[kMaxPacket];
    uint8_t out[kMaxPacket];
    const char* names[] = {"printer.local", "opta-settings.local"};
    size_t length = buildQuery(packet, 0x0000, names, 2, 1, 1);
    MdnsReply reply = buildMdnsAnswer(packet, static_cast<int>(length), 5353, "opta-settings",
        kIp, out, sizeof(out));
    ASSERT_ENUM(MdnsStatus::Answer, reply.status);
    // Указатель на имя второго вопроса: 12 + длина первого вопроса (15 + 4)
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[length]);
    TEST_ASSERT_EQUAL_HEX8(12 + 15 + 4, out[length + 1]);
}

static void test_mdns_ignores_bad_packets() {
    uint8_t packet[kMaxPacket];
    uint8_t out[kMaxPacket];
    const char* names[] = {"opta-settings.local"};
    size_t length = buildQuery(packet, 0x8400, names, 1, 1, 1);  // ответ
    ASSERT_ENUM(MdnsStatus::Ignore, buildMdnsAnswer(packet, static_cast<int>(length), 5353,
        "opta-settings", kIp, out, sizeof(out)).status);
    length = buildQuery(packet, 0x0000, names, 0, 1, 1);  // нет вопросов
    memset(packet + 12, 0, 8);
    ASSERT_ENUM(MdnsStatus::Ignore, buildMdnsAnswer(packet, 20, 5353,
        "opta-settings", kIp, out, sizeof(out)).status);
    length = buildQuery(packet, 0x0000, names, 1, 1, 1);
    ASSERT_ENUM(MdnsStatus::Ignore, buildMdnsAnswer(packet, static_cast<int>(length) - 3, 5353,
        "opta-settings", kIp, out, sizeof(out)).status);
    ASSERT_ENUM(MdnsStatus::Ignore, buildMdnsAnswer(packet, static_cast<int>(length), 5353,
        "opta-settings", kIp, out, length + 15).status);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_captive_answers_any_name);
    RUN_TEST(test_captive_ignores_bad_packets);
    RUN_TEST(test_mdns_answers_own_name);
    RUN_TEST(test_mdns_name_case_and_types);
    RUN_TEST(test_mdns_unicast);
    RUN_TEST(test_mdns_second_question_matches);
    RUN_TEST(test_mdns_ignores_bad_packets);
    return UNITY_END();
}
