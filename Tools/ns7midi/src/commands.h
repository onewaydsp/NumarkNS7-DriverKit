// commands.h
// Entry points for the ns7midi subcommands. Each takes the arguments after
// the subcommand name and returns the process exit status.

#pragma once

#include <string>
#include <vector>

int LearnCommand(const std::vector<std::string> & args);
int LedscanCommand(const std::vector<std::string> & args);
int VerifyCommand(const std::vector<std::string> & args);

// Shared by the commands.
std::string NowIso8601();
std::vector<std::string> SplitList(const std::string & text, char separator = ',');
