#include <cassert>
#include <fstream>
#include <iostream>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size);

std::string getFileContent(const std::string& path) {
	std::ifstream file(path);
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return content;
}

int main(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		auto input = getFileContent(argv[i]);
		LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.c_str()), input.size());
	}
}
