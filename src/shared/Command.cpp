/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 2013 Unvanquished Developers

This file is part of the Daemon GPL Source Code (Daemon Source Code).

Daemon Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "Command.h"

#include "../../engine/qcommon/qcommon.h"
#include "../../engine/framework/CommandSystem.h"

namespace Cmd {

    static CmdBase* firstCommand = nullptr;

    void Init() {
        for (CmdBase* cmd = firstCommand; cmd != nullptr; cmd = cmd->next) {
            AddCommand(cmd->GetCanonicalName(), cmd);
        }
    }

    std::string Escape(const std::string& text, bool quote) {
        std::string res;

        if (quote) {
            res = "\"";
        }

        for (int i = 0; i < text.size(); i ++) {
            char c = text[i];

            bool commentStart = (i < text.size() + 1) and (c == '/') and (text[i + 1] == '/' or text[i + 1] == '*');
            bool escapeNotQuote = (c <= ' ' and c > 0) or (c == ';') or commentStart;
            bool escape = (c == '$') or (c == '"') or (c == '\\');

            if ((not quote and escapeNotQuote) or escape) {
                res.push_back('\\');
            }
            res.push_back(c);
        }

        if (quote) {
            res += "\"";
        }

        return res;
    }

    void Tokenize(const std::string& text, std::vector<std::string>& tokens, std::vector<int>& tokenStarts) {
        const char* raw_text = text.c_str();
        std::string token;
        int tokenStart = 0;

        int pos = 0;
        bool inToken = false;

        while (pos < text.size()) {
            char c = text[pos ++];

            //Avoid whitespaces
            if (c <= ' ') {
                continue;
            }

            //Check for comments
            if (c == '/' and pos < text.size()) {
                char nextC = text[pos];

                // a // finishes the text and the token
                if (nextC == '/') {
                    break;

                // if it is a /* jump to the end of it
                } else if (nextC == '*'){
                    pos ++; //avoid /*/
                    while (pos < text.size() - 1 and not (text[pos] == '*' and text[pos + 1] == '/') ) {
                        pos ++;
                    }
                    pos += 2;

                    //The comment doesn't end
                    if (pos >= text.size()) {
                        break;
                    }
                    continue;
                }
            }

            //We have something that is not whitespace nor comments so it must be a token

            if (c == '"' and pos < text.size()) {
                //This is a quoted token
                bool escaped = false;

                c = text[pos ++]; //skips the "

                //Add all the characters (except the escape \) until the last unescaped "
                while (pos < text.size() and (escaped or c !='"')) {
                    if (escaped or c != '\\') {
                        token.push_back(c);
                        escaped = false;
                    } else if (not escaped) { // c = \ here
                        escaped = true;
                    }
                    c = text[pos ++];
                }

                tokens.push_back(token);
                tokenStarts.push_back(tokenStart);
                token = "";
                tokenStart = pos;

            } else {
                //An unquoted string, until the next " or comment start
                bool escaped = false;
                bool finished;
                bool startsSomethingElse;

                do {
                    if (escaped or c != '\\') {
                        token.push_back(c);
                        escaped = false;
                    } else if (not escaped) { // c = \ here
                        escaped = true;
                    }
                    c = text[pos ++];

                    startsSomethingElse = (
                        (pos < text.size() and c == '"') or //Start of a quote or start of a comment
                        (pos < text.size() and c == '/' and (text[pos] == '/' or text[pos] == '*'))
                    );

                    finished = not escaped and (c <= ' ' or startsSomethingElse);
                }while (pos < text.size() and not finished);

                //If we did not finish early (hitting another something)
                if(not escaped and not finished){
                    token.push_back(c);
                }

                tokens.push_back(token);
                tokenStarts.push_back(tokenStart);
                token = "";

                //Get back to the beginning of the seomthing
                if(startsSomethingElse){
                    pos--;
                }
                tokenStart = pos;
            }
        }

        //Handle the last token
        if (token != ""){
            tokens.push_back(token);
            tokenStarts.push_back(tokenStart);
        }
    }

    std::list<std::string> SplitCommands(const std::string& commands) {
        std::list<std::string> res;

        int commandStart = 0;
        bool inQuotes = false;
        bool escaped = false;

        //Splits are made on unquoted ; or newlines
        for(int i = 0; i < commands.size(); i++) {
            if (escaped) {
                escaped = false;
                continue;
            }

            char c = commands[i];

            if (c == '"') {
                inQuotes = not inQuotes;
                continue;
            }

            if (c == '\n' or (not inQuotes and c == ';')) {
                res.push_back(std::string(commands.c_str() + commandStart, i - commandStart));
                commandStart = i + 1;
            }
        }

        res.push_back(std::string(commands.c_str() + commandStart));

        return res;
    }

    std::string SubstituteCvars(const std::string& text) {
        const char* raw_text = text.c_str();
        std::string result;

        bool isEscaped = false;
        bool inCvarName = false;
        int lastBlockStart = 0;

        //Cvar are delimited by $ so we parse a bloc at a time
        for(int i = 0; i < text.size(); i++){

            // a \ escapes the next letter so we don't use it for block delimitation
            if (isEscaped) {
                isEscaped = false;
                continue;
            }

            if (text[i] == '\\') {
                isEscaped = true;

            } else if (text[i] == '$') {
                //Found a block, every second block is a cvar name block
                std::string block(raw_text + lastBlockStart, i - lastBlockStart);

                if (inCvarName) {
                    //For now we use the cvar C api to get the cvar value but it should be replaced
                    //by Cvar::get(cvarName)->getString() or something
                    char cvarValue[ MAX_CVAR_VALUE_STRING ];
                    Cvar_VariableStringBuffer( block.c_str(), cvarValue, sizeof( cvarValue ) );
                    result += std::string(cvarValue);

                    inCvarName = false;

                } else {
                    result += block;
                    inCvarName = true;
                }

                lastBlockStart = i + 1;
            }
        }

        //Handle the last block
        if (inCvarName) {
            Com_Printf("Warning: last CVar substitution block not closed in %s\n", raw_text);
        } else {
            std::string block(raw_text + lastBlockStart, text.size() - lastBlockStart);
            result += block;
        }

        return result;
    }

    /*
    ===============================================================================

    Cmd::CmdArgs

    ===============================================================================
    */

    Args::Args() {
    }

    Args::Args(const std::string& command) {
        Tokenize(command, args, argsStarts);
    }

    int Args::Argc() const {
        return args.size();
    }

    const std::string& Args::Argv(int argNum) const {
        return args[argNum];
    }

    std::string Args::QuotedArgs(int start, int end) const {
        std::string res;

        if (end < 0) {
            end = args.size() - 1;
        }

        for (int i = start; i < end + 1; i++) {
            if (i != start) {
                res += " ";
            }
            res += Escape(args[i], true);
        }

        return res;
    }

    std::string Args::OriginalArgs(int start, int end) const {
        int startOffset = argsStarts[start];
        int endOffset;

        if (end < 0) {
            endOffset = cmd.size();
        } else {
            endOffset = argsStarts[end];
        }

        return std::string(cmd.c_str() + startOffset, endOffset - startOffset);
    }

    int Args::ArgNumber(int pos) {
        for (int i = argsStarts.size(); i-->0;) {
            if (argsStarts[i] <= pos) {
                return i;
            }
        }

        return -1;
    }

    int Args::ArgStart(int argNum) {
        return argsStarts[argNum];
    }

    /*
    ===============================================================================

    Cmd::CmdBase

    ===============================================================================
    */

    CmdBase::CmdBase(const std::string name, const cmdFlags_t flags, const std::string description)
    :next(nullptr), name(name), description(description), flags(flags) {
        //Add this command to the static list of commands to be registered
        if (!(flags & NO_AUTO_REGISTER)) {
            this->next = firstCommand;
            firstCommand = this;
        }
    }

    std::vector<std::string> CmdBase::Complete(int argNum, const Args& args) const {
        return {};
    }

    const std::string& CmdBase::GetCanonicalName() const {
        return name;
    }

    const std::string& CmdBase::GetDescription() const {
        return description;
    }

    cmdFlags_t CmdBase::GetFlags() const {
        return flags;
    }

}
