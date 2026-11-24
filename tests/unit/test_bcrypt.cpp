// bcrypt against golang.org/x/crypto/bcrypt (what Syncthing stores) and OpenBSD's published vectors.
#include "core/bcrypt.hpp"
#include "test.hpp"

TEST(bcrypt_openbsd_vectors)
{
	// From OpenBSD's regress/lib/libc/bcrypt and the jBCrypt test suite.
	CHECK_EQ(xst::bcrypt::hash("", "$2a$06$DCq7YPn5Rq63x1Lad4cll."),
		"$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.");
	CHECK_EQ(xst::bcrypt::hash("abc", "$2a$06$If6bvum7DFjUnE9p2uDeDu"),
		"$2a$06$If6bvum7DFjUnE9p2uDeDu0YHzrHM6tf.iqN8.yx.jNN1ILEf7h0i");
	CHECK_EQ(xst::bcrypt::hash("abcdefghijklmnopqrstuvwxyz", "$2a$06$.rCVZVOThsIa97pEDOxvGu"),
		"$2a$06$.rCVZVOThsIa97pEDOxvGuRRgzG64bvtJ0938xuqzv18d3ZpQhstC");
	CHECK_EQ(xst::bcrypt::hash("~!@#$%^&*()      ~!@#$%^&*()PNBFRD", "$2a$06$fPIsBO8qRqkjj273rfaOI."),
		"$2a$06$fPIsBO8qRqkjj273rfaOI.HtSV9jLDpTbZn782DC6/t7qT67P6FfO");
}

TEST(bcrypt_generate_and_verify)
{
	const auto h = xst::bcrypt::generate("console", 4);
	CHECK(xst::bcrypt::looks_like_hash(h));
	CHECK(xst::bcrypt::verify("console", h));
	CHECK(!xst::bcrypt::verify("Console", h));
}
