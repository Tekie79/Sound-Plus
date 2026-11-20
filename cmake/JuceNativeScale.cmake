# Pinned JUCE 8.0.6: avoid repeating an unchanged IPlugViewContentScaleSupport
# notification after native attachment. Some macOS views apply backing scale
# again on that redundant call, separating drawing and input coordinates.
# Generic host policy; no plugin-name, state, identity or layout exceptions.
set(_sp_vst3 "${JUCE_DIR}/modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
file(READ "${_sp_vst3}" _sp_source)
if(NOT _sp_source MATCHES "SoundPlus unchanged content scale")
  set(_sp_old "            [[maybe_unused]] const auto result = scaleInterface->setContentScaleFactor ((IPlugViewContentScaleSupport::ScaleFactor) getEffectiveScale());")
  set(_sp_new "            // SoundPlus unchanged content scale: retain the view's native backing geometry.\n            const auto scale = getEffectiveScale();\n            if (lastContentScale.has_value() && *lastContentScale == scale)\n                return;\n            lastContentScale = scale;\n            [[maybe_unused]] const auto result = scaleInterface->setContentScaleFactor ((IPlugViewContentScaleSupport::ScaleFactor) scale);")
  string(FIND "${_sp_source}" "${_sp_old}" _sp_found)
  if(_sp_found EQUAL -1)
    message(FATAL_ERROR "Pinned JUCE VST3 scale implementation changed; review SoundPlus scale patch")
  endif()
  string(REPLACE "${_sp_old}" "${_sp_new}" _sp_source "${_sp_source}")
  string(REPLACE "    float userScaleFactor = 1.0f;" "    float userScaleFactor = 1.0f;\n    std::optional<float> lastContentScale;" _sp_source "${_sp_source}")
  file(WRITE "${_sp_vst3}" "${_sp_source}")
endif()
unset(_sp_source)
