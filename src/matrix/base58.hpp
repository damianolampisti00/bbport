#ifndef BASE58_HPP_
#define BASE58_HPP_

#include <QByteArray>
#include <QString>

// Bitcoin-alphabet Base58 decode, used to parse Matrix "Recovery Key" strings
// (e.g. "EsTc B4Xk ..."). Decode-only: BBport never needs to encode one.
class Base58
{
public:
    static bool decode(const QString &input, QByteArray *out);
};

#endif /* BASE58_HPP_ */
