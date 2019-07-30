#include "flow/serialize.h"

int main() {
	BinaryWriter wr(Unversioned());
	uint8_t x = 0;
	wr.serializeBinaryItem(x);
	int64_t y = 0;
	wr.serializeBinaryItem(y);
}
