// Copyright (c) 2014-2018 Zano Project
// Copyright (c) 2014-2018 The Louisdor Project
// Copyright (c) 2012-2013 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <fstream>

#include "include_base_utils.h"
#include "account.h"
#include "warnings.h"
#include "crypto/crypto.h"
#include "currency_core/currency_format_utils.h"
#include "common/mnemonic-encoding.h"

using namespace std;

//DISABLE_VS_WARNINGS(4244 4345)

namespace currency
{
  //-----------------------------------------------------------------
  account_base::account_base()
  {
    set_null();
  }
  //-----------------------------------------------------------------
  void account_base::set_null()
  {
    // fill sensitive data with random bytes
    crypto::generate_random_bytes(sizeof m_keys.spend_secret_key, &m_keys.spend_secret_key);
    crypto::generate_random_bytes(sizeof m_keys.view_secret_key, &m_keys.view_secret_key);
    if (m_keys_seed_binary.size())
      crypto::generate_random_bytes(m_keys_seed_binary.size(), &m_keys_seed_binary[0]);
    
    // clear
    m_keys = account_keys();
    m_creation_timestamp = 0;
    m_keys_seed_binary.clear();
  }
  //-----------------------------------------------------------------
  void account_base::generate(bool auditable /* = false */)
  {   
    if (auditable)
      m_keys.account_address.flags = ACCOUNT_PUBLIC_ADDRESS_FLAG_AUDITABLE;

    crypto::generate_seed_keys(m_keys.account_address.spend_public_key, m_keys.spend_secret_key, m_keys_seed_binary, BRAINWALLET_DEFAULT_SEED_SIZE);
    crypto::dependent_key(m_keys.spend_secret_key, m_keys.view_secret_key);
    if (!crypto::secret_key_to_public_key(m_keys.view_secret_key, m_keys.account_address.view_public_key))
      throw std::runtime_error("Failed to create public view key");


    m_creation_timestamp = time(NULL);
  }
  //-----------------------------------------------------------------
  const account_keys& account_base::get_keys() const
  {
    return m_keys;
  }
  //-----------------------------------------------------------------
  std::string account_base::get_seed_phrase() const 
  {
    if (m_keys_seed_binary.empty())
      return "";
    std::string keys_seed_text = tools::mnemonic_encoding::binary2text(m_keys_seed_binary);
    std::string timestamp_word = currency::get_word_from_timstamp(m_creation_timestamp);

    // floor creation time to WALLET_BRAIN_DATE_QUANTUM to make checksum calculation stable
    uint64_t creation_timestamp_rounded = get_timstamp_from_word(timestamp_word);

    constexpr uint16_t checksum_max = tools::mnemonic_encoding::NUMWORDS >> 1; // maximum value of checksum
    crypto::hash h = crypto::cn_fast_hash(m_keys_seed_binary.data(), m_keys_seed_binary.size());
    *reinterpret_cast<uint64_t*>(&h) = creation_timestamp_rounded;
    h = crypto::cn_fast_hash(&h, sizeof h);
    uint64_t h_64 = *reinterpret_cast<uint64_t*>(&h);
    uint16_t checksum = h_64 % (checksum_max + 1);
    
    uint8_t auditable_flag = 0;
    if (m_keys.account_address.flags & ACCOUNT_PUBLIC_ADDRESS_FLAG_AUDITABLE)
      auditable_flag = 1;

    uint32_t auditable_flag_and_checksum = (auditable_flag & 1) | (checksum << 1);
    std::string auditable_flag_and_checksum_word = tools::mnemonic_encoding::word_by_num(auditable_flag_and_checksum);

    return keys_seed_text + " " + timestamp_word + " " + auditable_flag_and_checksum_word;
  }
  //-----------------------------------------------------------------
  std::string account_base::get_tracking_seed() const
  {
    return get_public_address_str() + ":" +
      epee::string_tools::pod_to_hex(m_keys.view_secret_key) +
      (m_creation_timestamp ? ":" : "") + (m_creation_timestamp ? epee::string_tools::num_to_string_fast(m_creation_timestamp) : "");
  }
  //-----------------------------------------------------------------
  bool account_base::restore_keys(const std::vector<unsigned char>& keys_seed_binary)
  {
    CHECK_AND_ASSERT_MES(keys_seed_binary.size() == BRAINWALLET_DEFAULT_SEED_SIZE, false, "wrong restore data size: " << keys_seed_binary.size());
    crypto::keys_from_default(keys_seed_binary.data(), m_keys.account_address.spend_public_key, m_keys.spend_secret_key, keys_seed_binary.size());
    crypto::dependent_key(m_keys.spend_secret_key, m_keys.view_secret_key);
    bool r = crypto::secret_key_to_public_key(m_keys.view_secret_key, m_keys.account_address.view_public_key);
    CHECK_AND_ASSERT_MES(r, false, "failed to secret_key_to_public_key for view key");
    return true;
  }
  //-----------------------------------------------------------------
  bool account_base::restore_from_seed_phrase(const std::string& seed_phrase)
  {
    //cut the last timestamp word from restore_dats
    std::list<std::string> words;
    boost::split(words, seed_phrase, boost::is_space());
    
    std::string keys_seed_text, timestamp_word, auditable_flag_and_checksum_word;
    if (words.size() == SEED_PHRASE_V1_WORDS_COUNT)
    {
      // 24 seed words + one timestamp word = 25 total
      timestamp_word = words.back();
      words.erase(--words.end());
      keys_seed_text = boost::algorithm::join(words, " ");
    }
    else if (words.size() == SEED_PHRASE_V2_WORDS_COUNT)
    {
      // 24 seed words + one timestamp word + one flags & checksum = 26 total
      auditable_flag_and_checksum_word = words.back();
      words.erase(--words.end());
      timestamp_word = words.back();
      words.erase(--words.end());
      keys_seed_text = boost::algorithm::join(words, " ");
    }
    else
    {
      LOG_ERROR("Invalid seed words count: " << words.size());
      return false;
    }
    
    uint64_t auditable_flag_and_checksum = UINT64_MAX;
    if (!auditable_flag_and_checksum_word.empty())
      auditable_flag_and_checksum = tools::mnemonic_encoding::num_by_word(auditable_flag_and_checksum_word);

    std::vector<unsigned char> keys_seed_binary = tools::mnemonic_encoding::text2binary(keys_seed_text);
    CHECK_AND_ASSERT_MES(keys_seed_binary.size(), false, "text2binary failed to convert the given text"); // don't prints event incorrect seed into the log for security

    m_creation_timestamp = get_timstamp_from_word(timestamp_word);

    bool auditable_flag = false;

    // check the checksum if checksum word provided
    if (auditable_flag_and_checksum != UINT64_MAX)
    {
      auditable_flag = (auditable_flag_and_checksum & 1) != 0; // auditable flag is the lower 1 bit
      uint16_t checksum = static_cast<uint16_t>(auditable_flag_and_checksum >> 1); // checksum -- everything else
      constexpr uint16_t checksum_max = tools::mnemonic_encoding::NUMWORDS >> 1; // maximum value of checksum
      crypto::hash h = crypto::cn_fast_hash(keys_seed_binary.data(), keys_seed_binary.size());
      *reinterpret_cast<uint64_t*>(&h) = m_creation_timestamp;
      h = crypto::cn_fast_hash(&h, sizeof h);
      uint64_t h_64 = *reinterpret_cast<uint64_t*>(&h);
      uint16_t checksum_calculated = h_64 % (checksum_max + 1);
      CHECK_AND_ASSERT_MES(checksum == checksum_calculated, false, "seed phase has invalid checksum: " << checksum_calculated << ", while " << checksum << " is expected, check your words");
    }

    bool r = restore_keys(keys_seed_binary);
    CHECK_AND_ASSERT_MES(r, false, "restore_keys failed");

    m_keys_seed_binary = keys_seed_binary;

    if (auditable_flag)
      m_keys.account_address.flags |= ACCOUNT_PUBLIC_ADDRESS_FLAG_AUDITABLE;

    return true;
  }
  //-----------------------------------------------------------------
  bool account_base::restore_from_tracking_seed(const std::string& tracking_seed)
  {
    set_null();
    bool r = parse_tracking_seed(tracking_seed, m_keys.account_address, m_keys.view_secret_key, m_creation_timestamp);
    return r;
  }
  //-----------------------------------------------------------------
  std::string account_base::get_public_address_str() const
  {
    //TODO: change this code into base 58
    return get_account_address_as_str(m_keys.account_address);
  }
  //-----------------------------------------------------------------
  void account_base::make_account_watch_only()
  {
    // keep only:
    // timestamp
    // view pub & spend pub + flags (public address)
    // view sec
    
    // store to local tmp
    uint64_t local_ts = m_creation_timestamp;
    account_public_address local_addr = m_keys.account_address;
    crypto::secret_key local_view_sec = m_keys.view_secret_key;

    // clear
    set_null();

    // restore
    m_creation_timestamp = local_ts;
    m_keys.account_address = local_addr;
    m_keys.view_secret_key = local_view_sec;
  }
  //-----------------------------------------------------------------
  std::string transform_addr_to_str(const account_public_address& addr)
  {
    return get_account_address_as_str(addr);
  }
  //-----------------------------------------------------------------
  account_public_address transform_str_to_addr(const std::string& str)
  {
    account_public_address ad = AUTO_VAL_INIT(ad);
    if (!get_account_address_from_str(ad, str))
    {
      LOG_ERROR("cannot parse address from string: " << str);
    }
    return ad;
  }
}