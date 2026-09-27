/**
 * @brief Implementation file for UnixSignalHandler
 * 
 * This file exists to provide MOC (Meta Object Compiler) support
 * for the UnixSignalHandler class which uses Q_OBJECT.
 */

#include "UnixSignalHandler.hpp"

#if defined(Q_OS_UNIX) || defined(Q_OS_LINUX) || defined(Q_OS_MAC)
// Static member definitions
int UnixSignalHandler::sighupFd_[2] = {0, 0};
int UnixSignalHandler::sigtermFd_[2] = {0, 0};
int UnixSignalHandler::sigintFd_[2] = {0, 0};
#endif

// Include the MOC generated file for the header
// CMake AUTOMOC will generate this automatically
#include "moc_UnixSignalHandler.cpp"
