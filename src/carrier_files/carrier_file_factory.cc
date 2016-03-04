/**
* @file carrier_file_factory.cc
* @author Martin Kosdy
* @author Matus Kysel
* @date 2016
* @brief Factory for files
*
*/

#include "carrier_file_factory.h"

#include <algorithm>

#include "carrier_file.h"
#include "carrier_file_bmp.h"
#include "carrier_file_jpeg.h"
#include "encoders/encoder_factory.h"
#include "permutations/permutation_factory.h"
#include "utils/stego_config.h"

namespace stego_disk {

CarrierFilePtr CarrierFileFactory::CreateCarrierFile(const File& file) {
  shared_ptr<CarrierFile> carrier_file;

  std::string ext = file.GetExtension();
  if (ext.compare(".bmp") == 0) {
    carrier_file = std::make_shared<CarrierFileBMP>(file,
                                               nullptr,
                                               PermutationFactory::GetPermutation(StegoConfig::local_perm()),
                                               nullptr);
  } else if (ext.compare(".jpg") == 0) {
    carrier_file = std::make_shared<CarrierFileJPEG>(file,
                                                nullptr,
                                                PermutationFactory::GetPermutation(StegoConfig::local_perm()),
                                                nullptr);
  }

  if (carrier_file) {
    if (carrier_file->GetRawCapacity() < 1) {
      carrier_file = CarrierFilePtr();
    }
  }

  return carrier_file;
}

} // stego_disk
