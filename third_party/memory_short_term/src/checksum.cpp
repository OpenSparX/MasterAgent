#include "vehicle_memory/checksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace vehicle_memory {
namespace {

std::string ToLowerHex(const unsigned char* bytes, std::size_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return output.str();
}

#ifndef _WIN32

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t RotateRight(std::uint32_t value, std::uint32_t count) {
  return (value >> count) | (value << (32U - count));
}

std::string PortableSha256Hex(const std::string& bytes) {
  std::vector<unsigned char> message(bytes.begin(), bytes.end());
  const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while (message.size() % 64U != 56U) {
    message.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(
        static_cast<unsigned char>((bit_length >> shift) & 0xffU));
  }

  std::array<std::uint32_t, 8> state = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto position = offset + index * 4U;
      words[index] =
          (static_cast<std::uint32_t>(message[position]) << 24U) |
          (static_cast<std::uint32_t>(message[position + 1U]) << 16U) |
          (static_cast<std::uint32_t>(message[position + 2U]) << 8U) |
          static_cast<std::uint32_t>(message[position + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const auto s0 = RotateRight(words[index - 15U], 7U) ^
                      RotateRight(words[index - 15U], 18U) ^
                      (words[index - 15U] >> 3U);
      const auto s1 = RotateRight(words[index - 2U], 17U) ^
                      RotateRight(words[index - 2U], 19U) ^
                      (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^
                        RotateRight(e, 25U);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temporary1 =
          h + sum1 + choose + kSha256Constants[index] + words[index];
      const auto sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^
                        RotateRight(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  std::array<unsigned char, 32> digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4U] = static_cast<unsigned char>(state[index] >> 24U);
    digest[index * 4U + 1U] =
        static_cast<unsigned char>(state[index] >> 16U);
    digest[index * 4U + 2U] =
        static_cast<unsigned char>(state[index] >> 8U);
    digest[index * 4U + 3U] = static_cast<unsigned char>(state[index]);
  }
  return ToLowerHex(digest.data(), digest.size());
}

#endif

}  // namespace

std::string Sha256Hex(const std::string& bytes) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0;
  DWORD result_size = 0;
  DWORD hash_size = 0;

  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return {};
  }
  const auto close_algorithm = [&]() {
    if (hash != nullptr) {
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
  };
  if (!BCRYPT_SUCCESS(BCryptGetProperty(
          algorithm, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
          &result_size, 0)) ||
      !BCRYPT_SUCCESS(BCryptGetProperty(
          algorithm, BCRYPT_HASH_LENGTH,
          reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
          &result_size, 0))) {
    close_algorithm();
    return {};
  }

  std::vector<unsigned char> object(object_size);
  std::vector<unsigned char> digest(hash_size);
  if (!BCRYPT_SUCCESS(BCryptCreateHash(
          algorithm, &hash, object.data(),
          static_cast<ULONG>(object.size()), nullptr, 0, 0))) {
    close_algorithm();
    return {};
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const auto chunk = static_cast<ULONG>(std::min<std::size_t>(
        remaining, std::numeric_limits<ULONG>::max()));
    auto* data = reinterpret_cast<PUCHAR>(
        const_cast<char*>(bytes.data() + offset));
    if (!BCRYPT_SUCCESS(BCryptHashData(hash, data, chunk, 0))) {
      close_algorithm();
      return {};
    }
    offset += chunk;
  }
  if (!BCRYPT_SUCCESS(BCryptFinishHash(
          hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
    close_algorithm();
    return {};
  }
  close_algorithm();
  return ToLowerHex(digest.data(), digest.size());
#else
  return PortableSha256Hex(bytes);
#endif
}

}  // namespace vehicle_memory
