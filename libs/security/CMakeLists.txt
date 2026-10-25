# wt_security: Qt-free cryptography (Argon2id, ChaCha20-Poly1305) and the encrypted credential
# vault. Backend: OpenSSL >= 3.2 or libsodium (WT_CRYPTO_BACKEND, chosen at the top level); both
# read and write the same vault format.

add_library(wt_security STATIC
    include/wt/security/Crypto.h
    include/wt/security/Vault.h
    src/Vault.cpp)
add_library(wt::security ALIAS wt_security)

if(WT_CRYPTO_BACKEND_RESOLVED STREQUAL "openssl")
    target_sources(wt_security PRIVATE src/CryptoOpenSsl.cpp)
    target_link_libraries(wt_security PRIVATE OpenSSL::Crypto)
    target_compile_definitions(wt_security PUBLIC WT_CRYPTO_OPENSSL=1)
else()
    target_sources(wt_security PRIVATE src/CryptoSodium.cpp)
    target_link_libraries(wt_security PRIVATE WtSodium::sodium)
    target_compile_definitions(wt_security PUBLIC WT_CRYPTO_SODIUM=1)
endif()

target_include_directories(wt_security PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_security PUBLIC wt::core wt::platform)
wt_configure_target(wt_security)
