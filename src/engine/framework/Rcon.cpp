/*
===========================================================================
Daemon BSD Source Code
Copyright (c) 2013-2016, Daemon Developers
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Daemon developers nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DAEMON DEVELOPERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
===========================================================================
*/
#include "common/Common.h"
#include "engine/qcommon/qcommon.h"
#include "Rcon.h"
#include "Crypto.h"
#include "framework/Network.h"
#include "engine/server/CryptoChallenge.h"

namespace Rcon {

Cvar::Cvar<std::string> cvar_server_password(
    "rcon.server.password",
    "Password used to protect the remote console",
    Cvar::NONE,
    ""
);

Cvar::Range<Cvar::Cvar<int>> cvar_server_secure(
    "rcon.server.secure",
    "How secure the Rcon protocol should be: "
        "0: Allow unencrypted rcon, "
        "1: Require encryption, "
        "2: Require encryption and challenge check",
    Cvar::NONE,
    0,
    0,
    2
);

Cvar::Cvar<std::string> cvar_client_destination(
    "rcon.client.destination",
    "Destination address for rcon commands, if empty the current server.",
    Cvar::NONE,
    ""
);

Message::Message( const netadr_t& remote, std::string command,
				  Secure secure, std::string password, std::string challenge )
	: secure(secure),
	  challenge(std::move(challenge)),
	  command(std::move(command)),
	  password(std::move(password)),
	  remote(remote)
{}

Message::Message( std::string error_message )
	: error(std::move(error_message))
{}

const std::string& Message::Command() const
{
	return command;
}

void Message::Send() const
{
	if ( secure == Secure::Unencrypted )
	{
		Net::OutOfBandPrint(netsrc_t::NS_CLIENT, remote, "rcon %s %s", password, command);
		return;
	}

	std::string method = "PLAIN";
	Crypto::Data key = Crypto::Hash::Sha256(Crypto::String(password));
	std::string plaintext = command;

	if ( secure == Secure::EncryptedChallenge )
	{
		method = "CHALLENGE";
		plaintext = challenge + ' ' + plaintext;
	}

	Crypto::Data cypher;
	if ( Crypto::Aes256Encrypt(Crypto::String(plaintext), key, cypher) )
	{
		Net::OutOfBandPrint(netsrc_t::NS_CLIENT, remote, "srcon %s %s",
			method,
			Crypto::String(Crypto::Encoding::Base64Encode(cypher))
		);
	}
}

bool Message::Valid(std::string *invalid_reason) const
{
	auto invalid = [invalid_reason](const char* reason)
	{
		if ( invalid_reason )
			*invalid_reason = reason;
		return false;
	};

	if ( !error.empty() )
	{
		return invalid(error.c_str());
	}

	if ( secure < Secure::Unencrypted || secure > Secure::EncryptedChallenge )
	{
		return invalid("Unknown secure protocol");
	}

	if ( password.empty() )
	{
		return invalid("Missing password");
	}

	if ( command.empty() )
	{
		return invalid("Missing command");
	}

	if ( secure == Secure::EncryptedChallenge && challenge.empty() )
	{
		return invalid("Missing challenge");
	}

	return true;
}


bool Message::Acceptable(std::string *invalid_reason) const
{
	auto invalid = [invalid_reason](const char* reason)
	{
		if ( invalid_reason )
			*invalid_reason = reason;
		return false;
	};

	if ( !Valid(invalid_reason) )
	{
		return false;
	}

	if ( secure < Secure(cvar_server_secure.Get()) )
	{
		return invalid("Weak security");
	}

	if ( cvar_server_password.Get().empty() )
	{
		return invalid("No rcon.server.password set on the server.");
	}

	if ( password != cvar_server_password.Get() )
	{
		return invalid("Bad password");
	}

	if ( secure == Secure::EncryptedChallenge )
	{
		if ( !ChallengeManager::Get().MatchString(remote, challenge) )
		{
			return invalid("Mismatched challenge");
		}
	}

	return true;
}

Message Message::Decode(const netadr_t& remote, const Cmd::Args& args)
{
	if ( args.size() < 3 || (args[0] != "rcon" && args[0] != "srcon") )
	{
		return Message("Invalid command");
	}

	if ( cvar_server_password.Get().empty() )
	{
		return Message("rcon.server.password not set");
	}

	if ( args[0] == "rcon" )
	{
		return Message(remote, args.EscapedArgs(2), Secure::Unencrypted, args[1]);
	}

	auto authentication = args[1];
	Crypto::Data cyphertext = Crypto::String(args[2]);

	Crypto::Data data;
	if ( !Crypto::Encoding::Base64Decode( cyphertext, data ) )
	{
		return Message("Invalid Base64 string");
	}

	Crypto::Data key = Crypto::Hash::Sha256( Crypto::String( Rcon::cvar_server_password.Get() ) );

	if ( !Crypto::Aes256Decrypt( data, key, data ) )
	{
		return Message("Error during decryption");
	}

	std::string command = Crypto::String( data );

	if ( authentication == "CHALLENGE" )
	{
		std::istringstream stream( command );
		std::string challenge_hex;
		stream >> challenge_hex;

		while ( Str::cisspace( stream.peek() ) )
		{
			stream.ignore();
		}

		std::getline( stream, command );

		return Message(remote, command, Secure::EncryptedChallenge,
			Rcon::cvar_server_password.Get(), challenge_hex);
	}
	else if ( authentication == "PLAIN" )
	{
		return Message(remote, command, Secure::EncryptedPlain,
			Rcon::cvar_server_password.Get());
	}
	else
	{
		return Message(remote, command, Secure::Invalid,
			Rcon::cvar_server_password.Get());
	}
}

} // namespace Rcon
