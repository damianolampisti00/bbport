#include "base58.hpp"

#include <QVector>
#include <cstring>

static const char *kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool Base58::decode(const QString &input, QByteArray *out)
{
    if (!out) return false;
    out->clear();

    QByteArray digits;
    QByteArray raw = input.toLatin1();
    for (int i = 0; i < raw.size(); ++i) {
        char c = raw.at(i);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        digits.append(c);
    }
    if (digits.isEmpty()) return false;

    // Little-endian base-256 accumulator built by repeated multiply-add,
    // reversed into big-endian order at the end.
    QVector<int> bytes;
    bytes.append(0);

    for (int i = 0; i < digits.size(); ++i) {
        char c = digits.at(i);
        // strchr(s, '\0') returns a pointer to s's own terminator instead
        // of null -- without this explicit check, an embedded NUL byte
        // would silently decode as digit=58 (one past the alphabet's last
        // valid index) instead of being rejected like any other invalid
        // character.
        if (c == '\0') return false;
        const char *pos = std::strchr(kAlphabet, c);
        if (!pos) return false;
        int digit = int(pos - kAlphabet);

        int carry = digit;
        for (int j = 0; j < bytes.size(); ++j) {
            carry += bytes.at(j) * 58;
            bytes[j] = carry & 0xff;
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.append(carry & 0xff);
            carry >>= 8;
        }
    }

    int leadingZeros = 0;
    while (leadingZeros < digits.size() && digits.at(leadingZeros) == '1') leadingZeros++;

    for (int i = 0; i < leadingZeros; ++i) out->append(char(0));
    for (int i = bytes.size() - 1; i >= 0; --i) out->append(char(bytes.at(i)));
    return true;
}
