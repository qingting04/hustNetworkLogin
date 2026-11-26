/* Golden ePortal requests: run against the production encoder in CI. */
#include "eportal-request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_request(const char *password, const char *mac, const char *expected)
{
	char body[4096];
	size_t len = strlen(expected);

	assert(eportal_build_body(body, len + 1, "test-user", password, mac,
				  "a=1&b=%20 /+\xe6\xb1\x89") == 0);
	assert(strcmp(body, expected) == 0);
	assert(eportal_build_body(body, len, "test-user", password, mac,
				  "a=1&b=%20 /+\xe6\xb1\x89") == -1);
}

#define PREFIX "userId=test-user&password="
#define SUFFIX "&service=&queryString=a%3D1%26b%3D%2520%20%2F%2B%E6%B1%89&passwordEncrypt=true"

int main(void)
{
	test_request("123", "5ae915bf808f82732e98e01f704f00cd", PREFIX
		"91a0e02175f6a0b22ad23dac0d7f599806bc091f9fee1bfdada0d24d011dcdaed4"
		"18296b7c0ec560f988d92a7bb25dbf7ff51752d9bc6482a8180e56f7b772079ab"
		"59844abaae91e6d1c4660dc872717f9218f89acc9b70bb32891f28bf9d8f173d81"
		"b0e36c828deac919783e4e909ad1c22f953947b4a7ed7c90ac18fd95aa2" SUFFIX);
	test_request("test37", "02:00:00:00:00:01", PREFIX
		"00f933aa3d0e15287fb3cb20e8ed7c213844ab46b8cbc8ab5cfb1b40309b456b8"
		"5bc0a15c592e59d4aa8431904d4f9a3e9f73bd03671dba892a83f3c7b5fdfb439"
		"85cd916a190a50e316a1f40aaacf9964beaa920e74a137bc20df9a752e8cfcd200"
		"123c44ebf019f0ed62ef1b3033f932b73b0144a2f715570d44f6f67487b8" SUFFIX);
	puts("PASS: ePortal POST bytes, RSA zero padding, and body buffer boundary");
	return 0;
}
