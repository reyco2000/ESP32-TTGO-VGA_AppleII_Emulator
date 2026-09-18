/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : dsk_test.cpp
 *  Module : Host-side sector image test. Nibblizes .dsk / .po
 *           images with the real src/AppleII/DskImage.cpp and reads
 *           them back with an RWTS-style decoder, which is itself
 *           checked against the real .nib images in data/.
 *           Driven by tests/host/run-dsk-tests.sh.
 * ============================================================
*/

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "DskImage.h"

using namespace DskImage;

static int failures = 0;
static int checks = 0;

static void expect(bool ok, const std::string& what)
{
	checks++;
	if (!ok)
	{
		printf("  FAIL %s\n", what.c_str());
		failures++;
	}
}

// ---- decoder: what RWTS does, reading the track as a ring ----

static uint8_t readTable[256];

static void BuildReadTable()
{
	static const uint8_t WRITE_TABLE[64] = {
		0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
		0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
		0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
		0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
	};
	memset(readTable, 0xFF, sizeof(readTable));
	for (int i = 0; i < 64; i++)
		readTable[WRITE_TABLE[i]] = i;
}

struct TrackReader
{
	const uint8_t* nib;
	int pos;
	uint8_t Next() { uint8_t v = nib[pos]; pos = (pos + 1) % NIB_TRACK; return v; }
	uint8_t Get44() { uint8_t a = Next(); uint8_t b = Next(); return ((a << 1) | 1) & b; }
};

// Decodes every sector of one nibble track into sectors[phys]. Returns a
// bitmask of the physical sectors found with good checksums whose address
// field names 'track'.
static int DecodeTrack(const uint8_t* nib, int track, uint8_t sectors[16][256])
{
	int found = 0;
	for (int start = 0; start < NIB_TRACK; start++)
	{
		if (nib[start] != 0xD5 || nib[(start + 1) % NIB_TRACK] != 0xAA || nib[(start + 2) % NIB_TRACK] != 0x96)
			continue;
		TrackReader r = { nib, (start + 3) % NIB_TRACK };
		uint8_t vol = r.Get44(), trk = r.Get44(), sec = r.Get44(), sum = r.Get44();
		if ((vol ^ trk ^ sec) != sum || trk != track || sec > 15)
			continue;

		// data prologue within the next few dozen nibbles
		int n = 0;
		while (n < 64 && !(r.nib[r.pos] == 0xD5 && r.nib[(r.pos + 1) % NIB_TRACK] == 0xAA && r.nib[(r.pos + 2) % NIB_TRACK] == 0xAD))
		{
			r.Next();
			n++;
		}
		if (n == 64)
			continue;
		r.Next(); r.Next(); r.Next();

		uint8_t six[342];
		uint8_t prev = 0;
		bool bad = false;
		for (int i = 0; i < 342; i++)
		{
			uint8_t v = readTable[r.Next()];
			if (v == 0xFF) { bad = true; break; }
			prev ^= v;
			six[i] = prev;
		}
		if (bad || (readTable[r.Next()] ^ prev) != 0)
			continue;

		for (int i = 0; i < 256; i++)
		{
			int aux = six[i % 86] >> (2 * (i / 86));
			sectors[sec][i] = (six[86 + i] << 2) | ((aux & 1) << 1) | ((aux >> 1) & 1);
		}
		found |= 1 << sec;
	}
	return found;
}

// Whole nibble disk back to an image in the given order. False if any
// sector is missing.
static bool DecodeDisk(const uint8_t* nib, SectorOrder order, uint8_t* image)
{
	for (int t = 0; t < TRACKS; t++)
	{
		uint8_t sectors[16][256];
		if (DecodeTrack(nib + t * NIB_TRACK, t, sectors) != 0xFFFF)
			return false;
		for (int p = 0; p < 16; p++)
			memcpy(image + t * TRACK_BYTES + LogicalSector(p, order) * SECTOR_BYTES, sectors[p], SECTOR_BYTES);
	}
	return true;
}

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	std::vector<uint8_t> data;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return data;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	fclose(f);
	return data;
}

// Nibblizes image the way DiskIICard::InsertFloppy does: loaded at the tail
// of the drive buffer, converted in place.
static std::vector<uint8_t> NibblizeLikeTheCard(const uint8_t* image, SectorOrder order)
{
	std::vector<uint8_t> buf(NIB_BYTES, 0);
	memcpy(buf.data() + (NIB_BYTES - IMAGE_BYTES), image, IMAGE_BYTES);
	NibblizeInPlace(buf.data(), order);
	return buf;
}

// ---- tests ----

static void TestTypes()
{
	expect(TypeFromPath("/Games/karateka.nib") == IMAGE_NIB, "type .nib");
	expect(TypeFromPath("/DOS33.DSK") == IMAGE_DOS, "type .DSK");
	expect(TypeFromPath("/a/b.do") == IMAGE_DOS, "type .do");
	expect(TypeFromPath("/ProDOS.Po") == IMAGE_PRODOS, "type .Po");
	expect(TypeFromPath("/readme.txt") == IMAGE_NONE, "type .txt");
	expect(TypeFromPath(".po") == IMAGE_NONE, "type: bare extension");
	expect(TypeFromPath("/x.dsk.bak") == IMAGE_NONE, "type .dsk.bak");
}

// The decoder has to be right before it can judge the encoder: a DOS 3.3
// disk's VTOC (track 17, sector 0) has fixed fields in every bit position.
static void TestDecoderOnRealDisk(const std::string& dataDir)
{
	std::vector<uint8_t> nib = ReadFile(dataDir + "/u4-1.nib");
	expect(nib.size() == (size_t)NIB_BYTES, "u4-1.nib is a full .nib");
	if (nib.size() != (size_t)NIB_BYTES)
		return;

	uint8_t sectors[16][256];
	int found = DecodeTrack(nib.data() + 17 * NIB_TRACK, 17, sectors);
	expect(found & 1, "u4-1.nib: track 17 sector 0 decodes");
	const uint8_t* vtoc = sectors[0];
	expect(vtoc[0x01] == 17 && vtoc[0x02] == 15, "u4-1.nib VTOC: catalog at T17 S15");
	expect(vtoc[0x03] == 3, "u4-1.nib VTOC: DOS release 3");
	expect(vtoc[0x27] == 122, "u4-1.nib VTOC: 122 pairs per T/S list");
	expect(vtoc[0x34] == 35 && vtoc[0x35] == 16, "u4-1.nib VTOC: 35 tracks, 16 sectors");
	expect(vtoc[0x36] == 0x00 && vtoc[0x37] == 0x01, "u4-1.nib VTOC: 256 bytes per sector");
}

// Real disks: decode every standard sector, re-encode, decode again.
static void TestRealDisksRoundTrip(const std::string& dataDir)
{
	const char* names[] = { "karateka.nib", "loderunner.nib", "u4-1.nib", "u5-1.nib" };
	int roundTripped = 0;
	for (const char* name : names)
	{
		std::vector<uint8_t> nib = ReadFile(dataDir + "/" + name);
		if (nib.size() != (size_t)NIB_BYTES)
			continue;
		std::vector<uint8_t> image(IMAGE_BYTES);
		if (!DecodeDisk(nib.data(), ORDER_DOS, image.data()))
		{
			printf("  (%s is not all standard sectors, skipped)\n", name);
			continue;
		}
		std::vector<uint8_t> again = NibblizeLikeTheCard(image.data(), ORDER_DOS);
		std::vector<uint8_t> back(IMAGE_BYTES);
		expect(DecodeDisk(again.data(), ORDER_DOS, back.data()), std::string(name) + ": re-encoded disk decodes");
		expect(back == image, std::string(name) + ": re-encoded disk matches the original sectors");
		roundTripped++;
	}
	expect(roundTripped > 0, "at least one real disk round-tripped");
}

static void TestSynthetic(SectorOrder order, const char* label)
{
	// every sector distinct, every bit pattern present
	std::vector<uint8_t> image(IMAGE_BYTES);
	for (int i = 0; i < IMAGE_BYTES; i++)
		image[i] = (uint8_t)((i * 7) ^ (i >> 8) ^ (i >> 12) * 31);

	std::vector<uint8_t> nib = NibblizeLikeTheCard(image.data(), order);

	bool allHigh = true;
	for (uint8_t b : nib)
		allHigh &= (b & 0x80) != 0;
	expect(allHigh, std::string(label) + ": every nibble has bit 7 set");

	for (int t = 0; t < TRACKS; t++)
	{
		uint8_t sectors[16][256];
		int found = DecodeTrack(nib.data() + t * NIB_TRACK, t, sectors);
		if (found != 0xFFFF)
		{
			expect(false, std::string(label) + ": track " + std::to_string(t) + " has all 16 sectors");
			continue;
		}
		bool same = true;
		for (int p = 0; p < 16; p++)
			same &= memcmp(sectors[p], image.data() + t * TRACK_BYTES + LogicalSector(p, order) * SECTOR_BYTES, SECTOR_BYTES) == 0;
		expect(same, std::string(label) + ": track " + std::to_string(t) + " sectors match the image");
	}

	// the other order reads back a shuffled disk: the maps really differ
	std::vector<uint8_t> wrong(IMAGE_BYTES);
	DecodeDisk(nib.data(), order == ORDER_DOS ? ORDER_PRODOS : ORDER_DOS, wrong.data());
	expect(wrong != image, std::string(label) + ": the other sector order does not match");
}

static void TestOrders()
{
	// both maps are permutations of 0..15 that keep sector 0 in place (boot)
	for (SectorOrder o : { ORDER_DOS, ORDER_PRODOS })
	{
		int seen = 0;
		for (int p = 0; p < 16; p++)
			seen |= 1 << LogicalSector(p, o);
		expect(seen == 0xFFFF, "sector map is a permutation");
		expect(LogicalSector(0, o) == 0, "physical 0 holds sector 0");
	}
	// ProDOS block 0 is file sectors 0-1, on physical 0 and 2
	expect(LogicalSector(2, ORDER_PRODOS) == 1, "ProDOS: physical 2 holds sector 1");
	// DOS: RWTS logical sector 1 sits on physical 13
	expect(LogicalSector(13, ORDER_DOS) == 1, "DOS: physical 13 holds sector 1");
}

int main(int argc, char** argv)
{
	std::string dataDir = argc > 1 ? argv[1] : "data";
	BuildReadTable();

	TestTypes();
	TestOrders();
	TestDecoderOnRealDisk(dataDir);
	TestRealDisksRoundTrip(dataDir);
	TestSynthetic(ORDER_DOS, "DOS order");
	TestSynthetic(ORDER_PRODOS, "ProDOS order");

	if (failures)
	{
		printf("FAIL %d of %d checks\n", failures, checks);
		return 1;
	}
	printf("PASS %d checks\n", checks);
	return 0;
}
