#include "jbc/jbc.hpp"

#include <gtest/gtest.h>

#include <string_view>

TEST(LibJbcEntryTest, ReportsLibraryVersion)
{
    EXPECT_FALSE(std::string_view(jbc::get_lib_version()).empty());
    EXPECT_EQ(std::string_view(jbc::get_lib_version()), "0.4.23");
}
