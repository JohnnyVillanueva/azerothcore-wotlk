/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AuthSession.h"
#include "AES.h"
#include "AuthCodes.h"
#include "Config.h"
#include "CryptoGenerics.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "IPLocation.h"
#include "Log.h"
#include "RealmList.h"
#include "SecretMgr.h"
#include "StringConvert.h"
#include "TOTP.h"
#include "Util.h"
#include <boost/lexical_cast.hpp>

using boost::asio::ip::tcp;

enum eAuthCmd
{
    AUTH_LOGON_CHALLENGE = 0x00,
    AUTH_LOGON_PROOF = 0x01,
    AUTH_RECONNECT_CHALLENGE = 0x02,
    AUTH_RECONNECT_PROOF = 0x03,
    REALM_LIST = 0x10,
    XFER_INITIATE = 0x30,
    XFER_DATA = 0x31,
    XFER_ACCEPT = 0x32,
    XFER_RESUME = 0x33,
    XFER_CANCEL = 0x34
};

#pragma pack(push, 1)

typedef struct AUTH_LOGON_CHALLENGE_C
{
    uint8   cmd;
    uint8   error;
    uint16  size;
    uint8   gamename[4];
    uint8   version1;
    uint8   version2;
    uint8   version3;
    uint16  build;
    uint8   platform[4];
    uint8   os[4];
    uint8   country[4];
    uint32  timezone_bias;
    uint32  ip;
    uint8   I_len;
    uint8   I[1];
} sAuthLogonChallenge_C;
static_assert(sizeof(sAuthLogonChallenge_C) == (1 + 1 + 2 + 4 + 1 + 1 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 1 + 1));

typedef struct AUTH_LOGON_PROOF_C
{
    uint8   cmd;
    Acore::Crypto::SRP6::EphemeralKey A;
    Acore::Crypto::SHA1::Digest clientM;
    Acore::Crypto::SHA1::Digest crc_hash;
    uint8   number_of_keys;
    uint8   securityFlags;
} sAuthLogonProof_C;
static_assert(sizeof(sAuthLogonProof_C) == (1 + 32 + 20 + 20 + 1 + 1));

typedef struct AUTH_LOGON_PROOF_S
{
    uint8   cmd;
    uint8   error;
    Acore::Crypto::SHA1::Digest M2;
    uint32  AccountFlags;
    uint32  SurveyId;
    uint16  LoginFlags;
} sAuthLogonProof_S;
static_assert(sizeof(sAuthLogonProof_S) == (1 + 1 + 20 + 4 + 4 + 2));

typedef struct AUTH_LOGON_PROOF_S_OLD
{
    uint8   cmd;
    uint8   error;
    Acore::Crypto::SHA1::Digest M2;
    uint32  unk2;
} sAuthLogonProof_S_Old;
static_assert(sizeof(sAuthLogonProof_S_Old) == (1 + 1 + 20 + 4));

typedef struct AUTH_RECONNECT_PROOF_C
{
    uint8   cmd;
    uint8   R1[16];
    Acore::Crypto::SHA1::Digest R2, R3;
    uint8   number_of_keys;
} sAuthReconnectProof_C;
static_assert(sizeof(sAuthReconnectProof_C) == (1 + 16 + 20 + 20 + 1));

#pragma pack(pop)

std::array<uint8, 16> VersionChallenge = { { 0xBA, 0xA3, 0x1E, 0x99, 0xA0, 0x0B, 0x21, 0x57, 0xFC, 0x37, 0x3F, 0xB3, 0x69, 0xCD, 0xD2, 0xF1 } };

#define MAX_ACCEPTED_CHALLENGE_SIZE (sizeof(AUTH_LOGON_CHALLENGE_C) + 16)

#define AUTH_LOGON_CHALLENGE_INITIAL_SIZE 4
#define REALM_LIST_PACKET_SIZE 5

std::unordered_map<uint8, AuthHandler> AuthSession::InitHandlers()
{
    std::unordered_map<uint8, AuthHandler> handlers;

    handlers[AUTH_LOGON_CHALLENGE] =        { STATUS_CHALLENGE,         AUTH_LOGON_CHALLENGE_INITIAL_SIZE, &AuthSession::HandleLogonChallenge };
    handlers[AUTH_LOGON_PROOF] =            { STATUS_LOGON_PROOF,       sizeof(AUTH_LOGON_PROOF_C),        &AuthSession::HandleLogonProof };
    handlers[AUTH_RECONNECT_CHALLENGE] =    { STATUS_CHALLENGE,         AUTH_LOGON_CHALLENGE_INITIAL_SIZE, &AuthSession::HandleReconnectChallenge };
    handlers[AUTH_RECONNECT_PROOF] =        { STATUS_RECONNECT_PROOF,   sizeof(AUTH_RECONNECT_PROOF_C),    &AuthSession::HandleReconnectProof };
    handlers[REALM_LIST] =                  { STATUS_AUTHED,            REALM_LIST_PACKET_SIZE,            &AuthSession::HandleRealmList };

    return handlers;
}

std::unordered_map<uint8, AuthHandler> const Handlers = AuthSession::InitHandlers();

void AccountInfo::LoadResult(Field* fields)
{
    //          0        1          2           3             4             5
    // SELECT a.id, a.username, a.locked, a.lock_country, a.last_ip, a.failed_logins,
    //                                 6                                        7
    // ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, ab.unbandate = ab.bandate,
    //                                 8                                             9
    // ipb.unbandate > UNIX_TIMESTAMP() OR ipb.unbandate = ipb.bandate, ipb.unbandate = ipb.bandate,
    //      10
    // aa.gmlevel (, more query-specific fields)
    // FROM account a LEFT JOIN account_access aa ON a.id = aa.id LEFT JOIN account_banned ab ON ab.id = a.id AND ab.active = 1 LEFT JOIN ip_banned ipb ON ipb.ip = ? WHERE a.username = ?

    Id = fields[0].Get<uint32>();
    Login = fields[1].Get<std::string>();
    IsLockedToIP = fields[2].Get<bool>();
    LockCountry = fields[3].Get<std::string>();
    LastIP = fields[4].Get<std::string>();
    FailedLogins = fields[5].Get<uint32>();
    IsBanned = fields[6].Get<bool>() || fields[8].Get<bool>();
    IsPermanentlyBanned = fields[7].Get<bool>() || fields[9].Get<bool>();
    SecurityLevel = static_cast<AccountTypes>(fields[10].Get<uint8>()) > SEC_CONSOLE ? SEC_CONSOLE : static_cast<AccountTypes>(fields[10].Get<uint8>());

    // Use our own uppercasing of the account name instead of using UPPER() in mysql query
    // This is how the account was created in the first place and changing it now would result in breaking
    // login for all accounts having accented characters in their name
    Utf8ToUpperOnlyLatin(Login);
}

AuthSession::AuthSession(tcp::socket&& socket) :
    Socket(std::move(socket)), _status(STATUS_CHALLENGE), _build(0), _expversion(0) { }

void AuthSession::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    LOG_TRACE("session", "Accepted connection from {}", ip_address);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->SetData(0, ip_address);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&AuthSession::CheckIpCallback, this, std::placeholders::_1)));
}

bool AuthSession::Update()
{
    if (!AuthSocket::Update())
        return false;

    _queryProcessor.ProcessReadyCallbacks();

    return true;
}

void AuthSession::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;

        for (auto const& fields : *result)
        {
            if (fields[0].Get<uint64>() != 0)
            {
                banned = true;
                break;
            }
        }

        if (banned)
        {
            ByteBuffer pkt;
            pkt << uint8(AUTH_LOGON_CHALLENGE);
            pkt << uint8(0x00);
            pkt << uint8(WOW_FAIL_BANNED);
            SendPacket(pkt);
            LOG_DEBUG("session", "[AuthSession::CheckIpCallback] Banned ip '{}:{}' tries to login!", GetRemoteIpAddress().to_string(), GetRemotePort());
            return;
        }
    }

    AsyncRead();
}

void AuthSession::ReadHandler()
{
    MessageBuffer& packet = GetReadBuffer();

    while (packet.GetActiveSize())
    {
        uint8 cmd = packet.GetReadPointer()[0];
        auto itr = Handlers.find(cmd);
        if (itr == Handlers.end())
        {
            // well we dont handle this, lets just ignore it
            packet.Reset();
            break;
        }

        if (_status != itr->second.status)
        {
            CloseSocket();
            return;
        }

        uint16 size = uint16(itr->second.packetSize);
        if (packet.GetActiveSize() < size)
            break;

        if (cmd == AUTH_LOGON_CHALLENGE || cmd == AUTH_RECONNECT_CHALLENGE)
        {
            sAuthLogonChallenge_C* challenge = reinterpret_cast<sAuthLogonChallenge_C*>(packet.GetReadPointer());
            size += challenge->size;
            if (size > MAX_ACCEPTED_CHALLENGE_SIZE)
            {
                CloseSocket();
                return;
            }
        }

        if (packet.GetActiveSize() < size)
            break;

        if (!(*this.*itr->second.handler)())
        {
            CloseSocket();
            return;
        }

        packet.ReadCompleted(size);
    }

    AsyncRead();
}

void AuthSession::SendPacket(ByteBuffer& packet)
{
    if (!IsOpen())
        return;

    if (!packet.empty())
    {
        MessageBuffer buffer(packet.size());
        buffer.Write(packet.contents(), packet.size());
        QueuePacket(std::move(buffer));
    }
}

bool AuthSession::HandleLogonChallenge()
{
    _status = STATUS_CLOSED;

    sAuthLogonChallenge_C* challenge = reinterpret_cast<sAuthLogonChallenge_C*>(GetReadBuffer().GetReadPointer());
    if (challenge->size - (sizeof(sAuthLogonChallenge_C) - AUTH_LOGON_CHALLENGE_INITIAL_SIZE - 1) != challenge->I_len)
        return false;

    std::string login((char const*)challenge->I, challenge->I_len);
    LOG_DEBUG("server.authserver", "[AuthChallenge] '{}'", login);

    _build = challenge->build;
    _expversion = uint8(AuthHelper::IsPostBCAcceptedClientBuild(_build) ? POST_BC_EXP_FLAG : (AuthHelper::IsPreBCAcceptedClientBuild(_build) ? PRE_BC_EXP_FLAG : NO_VALID_EXP_FLAG));
    std::array<char, 5> os;
    os.fill('\0');
    memcpy(os.data(), challenge->os, sizeof(challenge->os));
    _os = os.data();

    // Restore string order as its byte order is reversed
    std::reverse(_os.begin(), _os.end());

    _localizationName.resize(4);
    for (int i = 0; i < 4; ++i)
        _localizationName[i] = challenge->country[4 - i - 1];

    // Get the account details from the account table
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_LOGONCHALLENGE);
    stmt->SetData(0, GetRemoteIpAddress().to_string());
    stmt->SetData(1, login);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&AuthSession::LogonChallengeCallback, this, std::placeholders::_1)));
    return true;
}

void AuthSession::LogonChallengeCallback(PreparedQueryResult result)
{
    ByteBuffer pkt;
    pkt << uint8(AUTH_LOGON_CHALLENGE);
    pkt << uint8(0x00);

    if (!result)
    {
        pkt << uint8(WOW_FAIL_UNKNOWN_ACCOUNT);
        SendPacket(pkt);
        return;
    }

    Field* fields = result->Fetch();

    _accountInfo.LoadResult(fields);

    std::string ipAddress = GetRemoteIpAddress().to_string();
    uint16 port = GetRemotePort();

    // If the IP is 'locked', check that the player comes indeed from the correct IP address
    if (_accountInfo.IsLockedToIP)
    {
        LOG_DEBUG("server.authserver", "[AuthChallenge] Account '{}' is locked to IP - '{}' is logging in from '{}'", _accountInfo.Login, _accountInfo.LastIP, ipAddress);
        if (_accountInfo.LastIP != ipAddress)
        {
            pkt << uint8(WOW_FAIL_LOCKED_ENFORCED);
            SendPacket(pkt);
            return;
        }
    }
    else
    {
        if (IpLocationRecord const* location = sIPLocation->GetLocationRecord(ipAddress))
            _ipCountry = location->CountryCode;

        LOG_DEBUG("server.authserver", "[AuthChallenge] Account '{}' is not locked to ip", _accountInfo.Login);
        if (_accountInfo.LockCountry.empty() || _accountInfo.LockCountry == "00")
            LOG_DEBUG("server.authserver", "[AuthChallenge] Account '{}' is not locked to country", _accountInfo.Login);
        else if (!_ipCountry.empty())
        {
            LOG_DEBUG("server.authserver", "[AuthChallenge] Account '{}' is locked to country: '{}' Player country is '{}'", _accountInfo.Login, _accountInfo.LockCountry, _ipCountry);
            if (_ipCountry != _accountInfo.LockCountry)
            {
                pkt << uint8(WOW_FAIL_UNLOCKABLE_LOCK);
                SendPacket(pkt);
                return;
            }
        }
    }

    // If the account is banned, reject the logon attempt
    if (_accountInfo.IsBanned)
    {
        if (_accountInfo.IsPermanentlyBanned)
        {
            pkt << uint8(WOW_FAIL_BANNED);
            SendPacket(pkt);
            LOG_INFO("server.authserver.banned", "'{}:{}' [AuthChallenge] Banned account {} tried to login!", ipAddress, port, _accountInfo.Login);
            return;
        }
        else
        {
            pkt << uint8(WOW_FAIL_SUSPENDED);
            SendPacket(pkt);
            LOG_INFO("server.authserver.banned", "'{}:{}' [AuthChallenge] Temporarily banned account {} tried to login!", ipAddress, port, _accountInfo.Login);
            return;
        }
    }

    uint8 securityFlags = 0;

    // Check if a TOTP token is needed
    if (!fields[11].IsNull())
    {
        securityFlags = 4;
        _totpSecret = fields[11].Get<Binary>();

        if (auto const& secret = sSecretMgr->GetSecret(SECRET_TOTP_MASTER_KEY))
        {
            bool success = Acore::Crypto::AEDecrypt<Acore::Crypto::AES>(*_totpSecret, *secret);
            if (!success)
            {
                pkt << uint8(WOW_FAIL_DB_BUSY);
                LOG_ERROR("server.authserver", "[AuthChallenge] Account '{}' has invalid ciphertext for TOTP token key stored", _accountInfo.Login);
                SendPacket(pkt);
                return;
            }
        }
    }

    _srp6.emplace(_accountInfo.Login,
        fields[12].Get<Binary, Acore::Crypto::SRP6::SALT_LENGTH>(),
        fields[13].Get<Binary, Acore::Crypto::SRP6::VERIFIER_LENGTH>());

    // Fill the response packet with the result
    if (AuthHelper::IsAcceptedClientBuild(_build))
    {
        pkt << uint8(WOW_SUCCESS);

        pkt.append(_srp6->B);
        pkt << uint8(1);
        pkt.append(_srp6->g);
        pkt << uint8(32);
        pkt.append(_srp6->N);
        pkt.append(_srp6->s);
        pkt.append(VersionChallenge.data(), VersionChallenge.size());
        pkt << uint8(securityFlags);            // security flags (0x0...0x04)

        if (securityFlags & 0x01)               // PIN input
        {
            pkt << uint32(0);
            pkt << uint64(0) << uint64(0);      // 16 bytes hash?
        }

        if (securityFlags & 0x02)               // Matrix input
        {
            pkt << uint8(0);
            pkt << uint8(0);
            pkt << uint8(0);
            pkt << uint8(0);
            pkt << uint64(0);
        }

        if (securityFlags & 0x04)               // Security token input
            pkt << uint8(1);

        LOG_DEBUG("server.authserver", "'{}:{}' [AuthChallenge] account {} is using '{}' locale ({})",
            ipAddress, port, _accountInfo.Login, _localizationName, GetLocaleByName(_localizationName));

        _status = STATUS_LOGON_PROOF;
    }
    else
        pkt << uint8(WOW_FAIL_VERSION_INVALID);

    SendPacket(pkt);
}

// Logon Proof command handler
bool AuthSession::HandleLogonProof()
{
    LOG_DEBUG("server.authserver", "Entering _HandleLogonProof");
    _status = STATUS_CLOSED;

    // Read the packet
    sAuthLogonProof_C* logonProof = reinterpret_cast<sAuthLogonProof_C*>(GetReadBuffer().GetReadPointer());

    // If the client has no valid version
    if (_expversion == NO_VALID_EXP_FLAG)
    {
        // Check if we have the appropriate patch on the disk
        LOG_DEBUG("network", "Client with invalid version, patching is not implemented");
        return false;
    }

    // Check if SRP6 results match (password is correct), else send an error
    if (Optional<SessionKey> K = _srp6->VerifyChallengeResponse(logonProof->A, logonProof->clientM))
    {
        _sessionKey = *K;
        // Check auth token
        bool tokenSuccess = false;
        bool sentToken = (logonProof->securityFlags & 0x04);
        if (sentToken && _totpSecret)
        {
            uint8 size = *(GetReadBuffer().GetReadPointer() + sizeof(sAuthLogonProof_C));
            std::string token(reinterpret_cast<char*>(GetReadBuffer().GetReadPointer() + sizeof(sAuthLogonProof_C) + sizeof(size)), size);
            GetReadBuffer().ReadCompleted(sizeof(size) + size);

            uint32 incomingToken = *Acore::StringTo<uint32>(token);
            tokenSuccess = Acore::Crypto::TOTP::ValidateToken(*_totpSecret, incomingToken);
            memset(_totpSecret->data(), 0, _totpSecret->size());
        }
        else if (!sentToken && !_totpSecret)
            tokenSuccess = true;

        if (!tokenSuccess)
        {
            ByteBuffer packet;
            packet << uint8(AUTH_LOGON_PROOF);
            packet << uint8(WOW_FAIL_UNKNOWN_ACCOUNT);
            packet << uint16(0);    // LoginFlags, 1 has account message
            SendPacket(packet);
            return true;
        }

        if (!VerifyVersion(logonProof->A.data(), logonProof->A.size(), logonProof->crc_hash, false))
        {
            ByteBuffer packet;
            packet << uint8(AUTH_LOGON_PROOF);
            packet << uint8(WOW_FAIL_VERSION_INVALID);
            SendPacket(packet);
            return true;
        }

        LOG_DEBUG("server.authserver", "'{}:{}' User '{}' successfully authenticated", GetRemoteIpAddress().to_string(), GetRemotePort(), _accountInfo.Login);

        // Update the sessionkey, last_ip, last login time and reset number of failed logins in the account table for this account
        // No SQL injection (escaped user name) and IP address as received by socket

        std::string address = sConfigMgr->GetOption<bool>("AllowLoggingIPAddressesInDatabase", true, true) ? GetRemoteIpAddress().to_string() : "0.0.0.0";
        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LOGONPROOF);
        stmt->SetData(0, _sessionKey);
        stmt->SetData(1, address);
        stmt->SetData(2, GetLocaleByName(_localizationName));
        stmt->SetData(3, _os);
        stmt->SetData(4, _accountInfo.Login);
        _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt)
            .WithPreparedCallback([this, M2 = Acore::Crypto::SRP6::GetSessionVerifier(logonProof->A, logonProof->clientM, _sessionKey)](PreparedQueryResult const&)
        {
            // Finish SRP6 and send the final result to the client
            ByteBuffer packet;
            if (_expversion & POST_BC_EXP_FLAG)                 // 2.x and 3.x clients
            {
                sAuthLogonProof_S proof;
                proof.M2 = M2;
                proof.cmd = AUTH_LOGON_PROOF;
                proof.error = 0;
                proof.AccountFlags = 0x00800000;    // 0x01 = GM, 0x08 = Trial, 0x00800000 = Pro pass (arena tournament)
                proof.SurveyId = 0;
                proof.LoginFlags = 0;               // 0x1 = has account message

                packet.resize(sizeof(proof));
                std::memcpy(packet.contents(), &proof, sizeof(proof));
            }
            else
            {
                sAuthLogonProof_S_Old proof;
                proof.M2 = M2;
                proof.cmd = AUTH_LOGON_PROOF;
                proof.error = 0;
                proof.unk2 = 0x00;

                packet.resize(sizeof(proof));
                std::memcpy(packet.contents(), &proof, sizeof(proof));
            }

            SendPacket(packet);
            _status = STATUS_AUTHED;
        }));
    }
    else
    {
        ByteBuffer packet;
        packet << uint8(AUTH_LOGON_PROOF);
        packet << uint8(WOW_FAIL_UNKNOWN_ACCOUNT);
        packet << uint16(0);    // LoginFlags, 1 has account message
        SendPacket(packet);

        LOG_INFO("server.authserver.hack", "'{}:{}' [AuthChallenge] account {} tried to login with invalid password!",
            GetRemoteIpAddress().to_string(), GetRemotePort(), _accountInfo.Login);

        uint32 MaxWrongPassCount = sConfigMgr->GetOption<int32>("WrongPass.MaxCount", 0);

        // We can not include the failed account login hook. However, this is a workaround to still log this.
        if (sConfigMgr->GetOption<bool>("WrongPass.Logging", false))
        {
            LoginDatabasePreparedStatement* logstmt = LoginDatabase.GetPreparedStatement(LOGIN_INS_FALP_IP_LOGGING);
            logstmt->SetData(0, _accountInfo.Id);
            logstmt->SetData(1, GetRemoteIpAddress().to_string());
            logstmt->SetData(2, "Login to WoW Failed - Incorrect Password");

            LoginDatabase.Execute(logstmt);
        }

        if (MaxWrongPassCount > 0)
        {
            //Increment number of failed logins by one and if it reaches the limit temporarily ban that account or IP
            LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_FAILEDLOGINS);
            stmt->SetData(0, _accountInfo.Login);
            LoginDatabase.Execute(stmt);

            if (++_accountInfo.FailedLogins >= MaxWrongPassCount)
            {
                uint32 WrongPassBanTime = sConfigMgr->GetOption<int32>("WrongPass.BanTime", 600);
                bool WrongPassBanType = sConfigMgr->GetOption<bool>("WrongPass.BanType", false);

                if (WrongPassBanType)
                {
                    stmt = LoginDatabase.GetPreparedStatement(LOGIN_INS_ACCOUNT_AUTO_BANNED);
                    stmt->SetData(0, _accountInfo.Id);
                    stmt->SetData(1, WrongPassBanTime);
                    LoginDatabase.Execute(stmt);

                    LOG_DEBUG("server.authserver", "'{}:{}' [AuthChallenge] account {} got banned for '{}' seconds because it failed to authenticate '{}' times",
                        GetRemoteIpAddress().to_string(), GetRemotePort(), _accountInfo.Login, WrongPassBanTime, _accountInfo.FailedLogins);
                }
                else
                {
                    stmt = LoginDatabase.GetPreparedStatement(LOGIN_INS_IP_AUTO_BANNED);
                    stmt->SetData(0, GetRemoteIpAddress().to_string());
                    stmt->SetData(1, WrongPassBanTime);
                    LoginDatabase.Execute(stmt);

                    LOG_DEBUG("server.authserver", "'{}:{}' [AuthChallenge] IP got banned for '{}' seconds because account {} failed to authenticate '{}' times",
                        GetRemoteIpAddress().to_string(), GetRemotePort(), WrongPassBanTime, _accountInfo.Login, _accountInfo.FailedLogins);
                }
            }
        }
    }

    return true;
}

bool AuthSession::HandleReconnectChallenge()
{
    _status = STATUS_CLOSED;

    sAuthLogonChallenge_C* challenge = reinterpret_cast<sAuthLogonChallenge_C*>(GetReadBuffer().GetReadPointer());
    if (challenge->size - (sizeof(sAuthLogonChallenge_C) - AUTH_LOGON_CHALLENGE_INITIAL_SIZE - 1) != challenge->I_len)
        return false;

    std::string login((char const*)challenge->I, challenge->I_len);
    LOG_DEBUG("server.authserver", "[ReconnectChallenge] '{}'", login);

    _build = challenge->build;
    _expversion = uint8(AuthHelper::IsPostBCAcceptedClientBuild(_build) ? POST_BC_EXP_FLAG : (AuthHelper::IsPreBCAcceptedClientBuild(_build) ? PRE_BC_EXP_FLAG : NO_VALID_EXP_FLAG));

    std::array<char, 5> os;
    os.fill('\0');
    memcpy(os.data(), challenge->os, sizeof(challenge->os));
    _os = os.data();

    // Restore string order as its byte order is reversed
    std::reverse(_os.begin(), _os.end());

    _localizationName.resize(4);
    for (int i = 0; i < 4; ++i)
        _localizationName[i] = challenge->country[4 - i - 1];

    // Get the account details from the account table
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_RECONNECTCHALLENGE);
    stmt->SetData(0, GetRemoteIpAddress().to_string());
    stmt->SetData(1, login);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&AuthSession::ReconnectChallengeCallback, this, std::placeholders::_1)));
    return true;
}

void AuthSession::ReconnectChallengeCallback(PreparedQueryResult result)
{
    ByteBuffer pkt;
    pkt << uint8(AUTH_RECONNECT_CHALLENGE);

    if (!result)
    {
        pkt << uint8(WOW_FAIL_UNKNOWN_ACCOUNT);
        SendPacket(pkt);
        return;
    }

    Field* fields = result->Fetch();

    _accountInfo.LoadResult(fields);
    _sessionKey = fields[11].Get<Binary, SESSION_KEY_LENGTH>();
    Acore::Crypto::GetRandomBytes(_reconnectProof);
    _status = STATUS_RECONNECT_PROOF;

    pkt << uint8(WOW_SUCCESS);
    pkt.append(_reconnectProof);
    pkt.append(VersionChallenge.data(), VersionChallenge.size());

    SendPacket(pkt);
}

bool AuthSession::HandleReconnectProof()
{
    LOG_DEBUG("server.authserver", "Entering _HandleReconnectProof");
    _status = STATUS_CLOSED;

    sAuthReconnectProof_C* reconnectProof = reinterpret_cast<sAuthReconnectProof_C*>(GetReadBuffer().GetReadPointer());

    if (_accountInfo.Login.empty())
        return false;

    Acore::Crypto::SHA1 sha;
    sha.UpdateData(_accountInfo.Login);
    sha.UpdateData(reconnectProof->R1, 16);
    sha.UpdateData(_reconnectProof);
    sha.UpdateData(_sessionKey);
    sha.Finalize();

    if (sha.GetDigest() == reconnectProof->R2)
    {
        if (!VerifyVersion(reconnectProof->R1, sizeof(reconnectProof->R1), reconnectProof->R3, true))
        {
            ByteBuffer packet;
            packet << uint8(AUTH_RECONNECT_PROOF);
            packet << uint8(WOW_FAIL_VERSION_INVALID);
            SendPacket(packet);
            return true;
        }

        // Sending response
        ByteBuffer pkt;
        pkt << uint8(AUTH_RECONNECT_PROOF);
        pkt << uint8(WOW_SUCCESS);
        pkt << uint16(0);    // LoginFlags, 1 has account message
        SendPacket(pkt);
        _status = STATUS_AUTHED;
        return true;
    }
    else
    {
        LOG_ERROR("server.authserver.hack", "'{}:{}' [ERROR] user {} tried to login, but session is invalid.", GetRemoteIpAddress().to_string(),
            GetRemotePort(), _accountInfo.Login);
        return false;
    }
}

bool AuthSession::HandleRealmList()
{
    LOG_DEBUG("server.authserver", "Entering _HandleRealmList");

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_REALM_CHARACTER_COUNTS);
    stmt->SetData(0, _accountInfo.Id);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&AuthSession::RealmListCallback, this, std::placeholders::_1)));
    _status = STATUS_WAITING_FOR_REALM_LIST;
    return true;
}

void AuthSession::RealmListCallback(PreparedQueryResult result)
{
    std::map<uint32, uint8> characterCounts;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            characterCounts[fields[0].Get<uint32>()] = fields[1].Get<uint8>();
        } while (result->NextRow());
    }

    // Circle through realms in the RealmList and construct the return packet (including # of user characters in each realm)
    ByteBuffer pkt;

    std::size_t RealmListSize = 0;
    for (auto const& [realmHandle, realm] : sRealmList->GetRealms())
    {
        // don't work with realms which not compatible with the client
        bool okBuild = ((_expversion & POST_BC_EXP_FLAG) && realm.Build == _build) || ((_expversion & PRE_BC_EXP_FLAG) && !AuthHelper::IsPreBCAcceptedClientBuild(realm.Build));

        // No SQL injection. id of realm is controlled by the database.
        uint32 flag = realm.Flags;
        RealmBuildInfo const* buildInfo = sRealmList->GetBuildInfo(realm.Build);
        if (!okBuild)
        {
            if (!buildInfo)
                continue;

            flag |= REALM_FLAG_OFFLINE | REALM_FLAG_SPECIFYBUILD;   // tell the client what build the realm is for
        }

        if (!buildInfo)
            flag &= ~REALM_FLAG_SPECIFYBUILD;

        std::string name = realm.Name;
        if (_expversion & PRE_BC_EXP_FLAG && flag & REALM_FLAG_SPECIFYBUILD)
        {
            std::ostringstream ss;
            ss << name << " (" << buildInfo->MajorVersion << '.' << buildInfo->MinorVersion << '.' << buildInfo->BugfixVersion << ')';
            name = ss.str();
        }

        uint8 lock = (realm.AllowedSecurityLevel > _accountInfo.SecurityLevel) ? 1 : 0;

        pkt << uint8(realm.Type);                           // realm type
        if (_expversion & POST_BC_EXP_FLAG)                 // only 2.x and 3.x clients
            pkt << uint8(lock);                             // if 1, then realm locked

        pkt << uint8(flag);                                 // RealmFlags
        pkt << name;
        pkt << boost::lexical_cast<std::string>(realm.GetAddressForClient(GetRemoteIpAddress()));
        pkt << float(realm.PopulationLevel);
        pkt << uint8(characterCounts[realm.Id.Realm]);
        pkt << uint8(realm.Timezone);                       // realm category

        if (_expversion & POST_BC_EXP_FLAG)                 // 2.x and 3.x clients
            pkt << uint8(realm.Id.Realm);
        else
            pkt << uint8(0x0);                              // 1.12.1 and 1.12.2 clients

        if (_expversion & POST_BC_EXP_FLAG && flag & REALM_FLAG_SPECIFYBUILD)
        {
            pkt << uint8(buildInfo->MajorVersion);
            pkt << uint8(buildInfo->MinorVersion);
            pkt << uint8(buildInfo->BugfixVersion);
            pkt << uint16(buildInfo->Build);
        }

        ++RealmListSize;
    }

    if (_expversion & POST_BC_EXP_FLAG)                     // 2.x and 3.x clients
    {
        pkt << uint8(0x10);
        pkt << uint8(0x00);
    }
    else                                                    // 1.12.1 and 1.12.2 clients
    {
        pkt << uint8(0x00);
        pkt << uint8(0x02);
    }

    // make a ByteBuffer which stores the RealmList's size
    ByteBuffer RealmListSizeBuffer;
    RealmListSizeBuffer << uint32(0);

    if (_expversion & POST_BC_EXP_FLAG)                     // only 2.x and 3.x clients
        RealmListSizeBuffer << uint16(RealmListSize);
    else
        RealmListSizeBuffer << uint32(RealmListSize);

    ByteBuffer hdr;
    hdr << uint8(REALM_LIST);
    hdr << uint16(pkt.size() + RealmListSizeBuffer.size());
    hdr.append(RealmListSizeBuffer);                        // append RealmList's size buffer
    hdr.append(pkt);                                        // append realms in the realmlist
    SendPacket(hdr);

    _status = STATUS_AUTHED;
}

bool AuthSession::VerifyVersion(uint8 const* a, int32 aLength, Acore::Crypto::SHA1::Digest const& versionProof, bool isReconnect)
{
    if (!sConfigMgr->GetOption<bool>("StrictVersionCheck", false))
        return true;

    Acore::Crypto::SHA1::Digest zeros{};
    Acore::Crypto::SHA1::Digest const* versionHash{ nullptr };

    if (!isReconnect)
    {
        RealmBuildInfo const* buildInfo = sRealmList->GetBuildInfo(_build);
        if (!buildInfo)
            return false;

        if (_os == "Win")
            versionHash = &buildInfo->WindowsHash;
        else if (_os == "OSX")
            versionHash = &buildInfo->MacHash;

        if (!versionHash)
            return false;

        if (zeros == *versionHash)
            return true;                                                            // not filled serverside
    }
    else
        versionHash = &zeros;

    Acore::Crypto::SHA1 version;
    version.UpdateData(a, aLength);
    version.UpdateData(*versionHash);
    version.Finalize();

    return (versionProof == version.GetDigest());
}
