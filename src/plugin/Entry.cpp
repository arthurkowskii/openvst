// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new openvst::Processor(OPENVST_IS_INSTRUMENT != 0); }
