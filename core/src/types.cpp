#include "mf/types.h"

namespace mf {

const char* toString(Result r) {
  switch (r) {
    case Result::Ok: return "Ok";
    case Result::InvalidState: return "InvalidState";
    case Result::WrongThread: return "WrongThread";
    case Result::InvalidArgument: return "InvalidArgument";
    case Result::FileOpenFailed: return "FileOpenFailed";
    case Result::UnsupportedFormat: return "UnsupportedFormat";
    case Result::NoDecoder: return "NoDecoder";
    case Result::MalformedMedia: return "MalformedMedia";
    case Result::DecoderFailed: return "DecoderFailed";
    case Result::AudioDeviceFailed: return "AudioDeviceFailed";
    case Result::Again: return "Again";
    case Result::Eos: return "Eos";
    case Result::CorruptFrame: return "CorruptFrame";
  }
  return "?";
}

const char* toString(State s) {
  switch (s) {
    case State::Start: return "START";
    case State::Ready: return "READY";
    case State::Play: return "PLAY";
    case State::Error: return "ERROR";
    case State::Shutdown: return "SHUTDOWN";
  }
  return "?";
}

const char* toString(Warning w) {
  switch (w) {
    case Warning::AudioUnsupported: return "AudioUnsupported";
    case Warning::RotationIgnored: return "RotationIgnored";
  }
  return "?";
}

}  // namespace mf
