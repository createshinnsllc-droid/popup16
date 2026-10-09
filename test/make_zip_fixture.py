# Builds a tiny .zip whose only member has a literal [!] in its name. unzip treats [ ] * ?
# as wildcards, so extracting that name needs escaping. The body is deterministic dummy data.
import sys
import zipfile

MEMBER = "Test Game (U) [!].sfc"
BODY = bytes((i * 31 + 7) & 0xFF for i in range(0x8000))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_zip_fixture.py OUT.zip")
    info = zipfile.ZipInfo(MEMBER, date_time=(2024, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    with zipfile.ZipFile(sys.argv[1], "w") as archive:
        archive.writestr(info, BODY)
