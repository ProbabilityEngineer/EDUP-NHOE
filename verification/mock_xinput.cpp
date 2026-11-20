#include "mock_api.hpp"

namespace {
SRWLOCK lock = SRWLOCK_INIT;
MockSnapshot snapshot{};
DWORD stop_failures = 0;
bool stop_preserves_error = false;
HANDLE entered = nullptr;
HANDLE release = nullptr;
}

extern "C" void WINAPI MockReset() {
  AcquireSRWLockExclusive(&lock);
  snapshot = {};
  if (!entered) entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!release) release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ResetEvent(entered); ResetEvent(release);
  ReleaseSRWLockExclusive(&lock);
}

extern "C" void WINAPI MockFailStops(DWORD count) {
  AcquireSRWLockExclusive(&lock); stop_failures = count & 0x7fffffff; stop_preserves_error = (count & 0x80000000) != 0; ReleaseSRWLockExclusive(&lock);
}

extern "C" void WINAPI MockRead(MockSnapshot* result) {
  AcquireSRWLockShared(&lock); *result = snapshot; ReleaseSRWLockShared(&lock);
}
extern "C" DWORD WINAPI MockWaitUntilBlocked(DWORD timeout) { return WaitForSingleObject(entered, timeout); }
extern "C" void WINAPI MockRelease() { SetEvent(release); }

extern "C" DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION* vibration) {
  const DWORD incoming_error = GetLastError();
  AcquireSRWLockExclusive(&lock);
  ++snapshot.set_calls;
  snapshot.index = index; snapshot.pointer = reinterpret_cast<ULONG_PTR>(vibration); snapshot.incoming_error = incoming_error;
  snapshot.left = 0; snapshot.right = 0;
  if (vibration && reinterpret_cast<ULONG_PTR>(vibration) != 1) { snapshot.left = vibration->wLeftMotorSpeed; snapshot.right = vibration->wRightMotorSpeed; }
  const bool fail = index == 1 && !snapshot.left && !snapshot.right && stop_failures;
  const bool preserve = stop_preserves_error;
  if (fail) --stop_failures;
  ReleaseSRWLockExclusive(&lock);
  if (fail) { SetLastError(preserve ? incoming_error : 0x77665544); return ERROR_BUSY; }
  if (index == 42) { SetEvent(entered); WaitForSingleObject(release, 10000); }
  if (!vibration) { SetLastError(0x55aa0000); return ERROR_INVALID_PARAMETER; }
  if (reinterpret_cast<ULONG_PTR>(vibration) == 1) { SetLastError(0x55aa0001); return ERROR_NOACCESS; }
  if (index == 7) { vibration->wLeftMotorSpeed = 0xbeef; vibration->wRightMotorSpeed = 0x1357; SetLastError(0x4040); return 0x76543210; }
  SetLastError(0x88776655);
  if (index == 2) return ERROR_BUSY;
  if (index == 99) return ERROR_INVALID_PARAMETER;
  return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE* state) {
  const DWORD error = GetLastError();
  AcquireSRWLockExclusive(&lock);
  snapshot.index = index; snapshot.pointer = reinterpret_cast<ULONG_PTR>(state); snapshot.incoming_error = error;
  ReleaseSRWLockExclusive(&lock);
  if (state) *state = {0x13579bdf, {0x246, 0x12, 0x34, -1234, 2345, -3456, 4567}};
  SetLastError(0x11223344); return 0x10203040;
}
extern "C" DWORD WINAPI MockAlternateSet(DWORD index, XINPUT_VIBRATION* vibration) {
  return XInputSetState(index,vibration);
}

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES* capabilities) {
  const DWORD error = GetLastError();
  AcquireSRWLockExclusive(&lock);
  snapshot.index = index; snapshot.flags = flags; snapshot.pointer = reinterpret_cast<ULONG_PTR>(capabilities); snapshot.incoming_error = error;
  ReleaseSRWLockExclusive(&lock);
  if (capabilities) { *capabilities = {}; capabilities->Type = 1; capabilities->SubType = 2; capabilities->Flags = 0x1357; capabilities->Vibration = {0x2468, 0xabcd}; }
  SetLastError(0x22334455); return 0x20304050;
}

extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID* render, GUID* capture) {
  const DWORD error = GetLastError();
  AcquireSRWLockExclusive(&lock);
  snapshot.index = index; snapshot.pointer = reinterpret_cast<ULONG_PTR>(render); snapshot.second_pointer = reinterpret_cast<ULONG_PTR>(capture); snapshot.incoming_error = error;
  ReleaseSRWLockExclusive(&lock);
  if (render) *render = {0x10203040, 0x5060, 0x7080, {1, 2, 3, 4, 5, 6, 7, 8}};
  if (capture) *capture = {0xabcdef01, 0x2345, 0x6789, {8, 7, 6, 5, 4, 3, 2, 1}};
  SetLastError(0x33445566); return 0x30405060;
}
