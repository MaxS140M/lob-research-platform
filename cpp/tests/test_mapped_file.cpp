#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "lob/mapped_file.hpp"

namespace fs = std::filesystem;

namespace {

class MappedFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = fs::temp_directory_path() /
                ("lob_mapped_file_" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    }
    void TearDown() override { fs::remove(path_); }

    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    fs::path path_;
};

}  // namespace

TEST_F(MappedFileTest, MapsFileContents) {
    const std::vector<std::uint8_t> bytes = {0x00, 0x0C, 'S', 0xFF, 0x10};
    write(bytes);

    const lob::MappedFile file(path_);
    ASSERT_EQ(file.size(), bytes.size());
    const auto view = file.bytes();
    EXPECT_EQ(std::vector<std::uint8_t>(view.begin(), view.end()), bytes);
}

TEST_F(MappedFileTest, EmptyFileGivesEmptySpan) {
    write({});
    const lob::MappedFile file(path_);
    EXPECT_EQ(file.size(), 0U);
    EXPECT_TRUE(file.bytes().empty());
}

TEST_F(MappedFileTest, MoveTransfersOwnership) {
    write({1, 2, 3});
    lob::MappedFile a(path_);
    lob::MappedFile b(std::move(a));
    EXPECT_EQ(b.size(), 3U);
    EXPECT_EQ(a.size(), 0U);  // NOLINT(bugprone-use-after-move): moved-from state is defined
}

TEST(MappedFile, MissingFileThrows) {
    EXPECT_THROW(lob::MappedFile("/nonexistent/lob/file.itch"), std::system_error);
}
