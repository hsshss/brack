#pragma once
#include <future>
#include <string>

struct GLFWwindow;

namespace brack {
// Native file dialogs, over `owner` once it is set. The future gives a UTF-8 path, or "" when
// cancelled. The dialog runs on a thread of its own, so that the GUI goes on drawing; one at a time.
void setFileDialogOwner(GLFWwindow* owner);
std::future<std::string> openFileDialog(std::string filterName, std::string pattern);
std::future<std::string> saveFileDialog(std::string filterName, std::string pattern, std::string defaultExt);
}  // namespace brack
