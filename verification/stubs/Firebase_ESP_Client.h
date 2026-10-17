#pragma once
#include <Arduino.h>
struct FirebaseData { String errorReason() { return String(); } };
struct FirebaseAuth {};
struct TokenInfo;
struct FirebaseSignupError { std::string message; };
struct FirebaseSigner { FirebaseSignupError signupError; };
struct FirebaseConfig {
  std::string api_key; std::string database_url;
  void (*token_status_callback)(TokenInfo) = nullptr;
  FirebaseSigner signer;
  struct { int serverResponse = 0; int socketConnection = 0; } timeout;
};
struct FirebaseJson {
  void set(const char*, int) {}
  void set(const char*, float) {}
  void set(const char*, bool) {}
  void set(const char*, const char*) {}
};
struct RTDBStub {
  bool setInt(FirebaseData*, const char*, int);
  bool setFloat(FirebaseData*, const char*, float);
  bool setString(FirebaseData*, const char*, const char*);
  bool setBool(FirebaseData*, const char*, bool);
  bool updateNodeSilent(FirebaseData*, const char*, FirebaseJson*);
};
struct FirebaseStub {
  RTDBStub RTDB;
  bool signUp(FirebaseConfig*, FirebaseAuth*, const char*, const char*);
  void begin(FirebaseConfig*, FirebaseAuth*);
  void reconnectWiFi(bool);
  bool ready();
};
extern FirebaseStub Firebase;
