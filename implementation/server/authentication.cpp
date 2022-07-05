#include "authentication.h"
#include "../common/dhparams.h"
#include "../common/errors.h"
#include "../common/utils.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <openssl/aes.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <string.h>
#include <tuple>

using namespace std;

void free_user_keys(map<string, EVP_PKEY *> keys) {
    for (auto it = keys.begin(); it != keys.end(); it++) {
        EVP_PKEY_free(it->second);
    }
    keys.clear();
}

// Possible users of the server
string users[2] = {"alice", "bob"};
char server_name[] = "server";

/* Reads all the public keys of the registered users */
static map<string, EVP_PKEY *> setup_keys() {
    auto user_map = map<string, EVP_PKEY *>();
    FILE *public_key_fp;
    EVP_PKEY *pubkey;

    for (string user : users) {

        // Get the user public key path
        auto user_key_path =
            (std::filesystem::canonical(".") / "certificates" / (user + ".pub"))
                .string();

        // Open the public key file
        if ((public_key_fp = fopen(user_key_path.c_str(), "r")) == nullptr) {
            user_map.clear();
            perror("Cannot read user key");
            abort();
        }

        // ... and read it as a public key
        if ((pubkey = PEM_read_PUBKEY(public_key_fp, nullptr, 0, nullptr)) ==
            nullptr) {
            free_user_keys(user_map);
            handle_errors();
        }

        fclose(public_key_fp);

        // ... and save it into the list of users
        user_map.insert({user, pubkey});

#ifdef DEBUG
        cout << "Loaded public key for " << user << endl
             << "Path: " << user_key_path << endl
             << endl;
#endif
    }

    return user_map;
}

/*
 * Runs the key agreement protocol with the client.
 * Returns a tuple containing the username of the client and the agreed key.
 * The caller of this function has to free the memory allocated for the key when
 * done with it.
 */
tuple<char *, unsigned char *> authenticate(int socket, int key_len) {
    // Setup simple associations of usernames and public keys
    auto user_keys = setup_keys();

    // ---------------------------------------------------------------------- //
    // ----------------- Client's opening message to Server ----------------- //
    // ---------------------------------------------------------------------- //

    // Receive first client message
    auto client_header_result = get_mtype(socket);

    // Check the correctness of the message type
    if (client_header_result.is_error ||
        client_header_result.result != AuthStart) {
        free_user_keys(user_keys);
        handle_errors("Incorrect message type");
    }

    // Read the username of the client
    auto username_result = read_field<char>(socket);
    if (username_result.is_error) {
        free_user_keys(user_keys);
        handle_errors(username_result.error);
    }
    auto [username_len, username] = username_result.result;
    username[username_len - 1] = '\0';

#ifdef DEBUG
    cout << endl << "Username length: " << username_len << endl;
    cout << "Username: " << username << endl << endl;
#endif

    // Check that it is registered on the server
    auto finder = user_keys.find(username);
    if (finder != user_keys.end()) {
        auto client_pubkey = finder->second;
    } else {
        free_user_keys(user_keys);
        delete[] username;
        username = nullptr;
        handle_errors("User not registered!");
    }

    // Load the client's half key
    BIO *tmp_bio;
    if ((tmp_bio = BIO_new(BIO_s_mem())) == nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        username = nullptr;
        handle_errors("Could not allocate memory bio");
    }

    // Read client half key in PEM format
    auto half_key_result = read_field<uchar>(socket);
    if (half_key_result.is_error) {
        free_user_keys(user_keys);
        delete[] username;
        username = nullptr;
        BIO_free(tmp_bio);
        handle_errors(half_key_result.error);
    }
    auto [client_half_key_len, client_half_key_pem] = half_key_result.result;

    // Write it to memory bio
    if (BIO_write(tmp_bio, client_half_key_pem, client_half_key_len) !=
        client_half_key_len) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        handle_errors("Could not write to memory bio");
    }

    // ... and extract it as the client half key
    auto client_half_key = PEM_read_bio_PUBKEY(tmp_bio, nullptr, 0, nullptr);
    if (client_half_key == nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        handle_errors("Could not read from memory bio");
    }
    BIO_reset(tmp_bio);

#ifdef DEBUG
    cout << "Client half key:" << endl;
    PEM_write_PUBKEY(stdout, client_half_key);
    cout << endl;
#endif

    // ---------------------------------------------------------------------- //
    // --------------------- Server's response to client -------------------- //
    // ---------------------------------------------------------------------- //

    // Send header
    auto send_header_result = send_header(socket, AuthServerAns);
    if (send_header_result.is_error) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        handle_errors(send_header_result.error);
    }

    // Send server name ("server")
    auto send_server_name_res =
        send_field(socket, sizeof(server_name), server_name);
    if (send_server_name_res.is_error) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        handle_errors(send_server_name_res.error);
    }

    // Send server's half key

    // Generate the keypair for the server and write it the public key to the
    // memory bio to extract it as PEM
    auto keypair = gen_keypair();
    if (PEM_write_bio_PUBKEY(tmp_bio, keypair) != 1) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not write to memory bio");
    };

    // Get the length and a pointer to the bio's memory data
    long server_half_key_len;
    char *server_half_key_ptr;
    if ((server_half_key_len =
             BIO_get_mem_data(tmp_bio, &server_half_key_ptr)) <= 0) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not read from memory bio");
    }

#ifdef DEBUG
    cout << "Server half key:" << endl;
    PEM_write_PUBKEY(stdout, client_half_key);
    cout << endl;
#endif

    // Check if the size of the public key is less than the maximum size of a
    // packet field
    if (server_half_key_len > FLEN_MAX) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Server's half key length is bigger than the maximum "
                      "field's length");
    }

    // Copy the half key for later usage (signature computation/verification)
    char *server_half_key_pem = new char[server_half_key_len];
    memcpy(server_half_key_pem, server_half_key_ptr, server_half_key_len);

    // Actually send the half key
    auto send_server_half_key_result = send_field<char>(
        socket, (flen)server_half_key_len, server_half_key_ptr);

    // and check the result
    if (send_server_half_key_result.is_error) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        delete[] server_half_key_pem;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors(send_server_half_key_result.error);
    }
    BIO_reset(tmp_bio);

    // Send server's certificate

    // Open the certificate file
    FILE *server_certificate_fp;
    if ((server_certificate_fp = fopen("certificates/server.crt", "r")) ==
        nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        delete[] server_half_key_pem;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not open server's certificate file");
    }

    // Read the certificate into an X509 struct
    X509 *server_certificate;
    if ((server_certificate = PEM_read_X509(server_certificate_fp, nullptr, 0,
                                            nullptr)) == nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        fclose(server_certificate_fp);
        handle_errors("Could not read X509 certificate from file");
    }

    fclose(server_certificate_fp);

    // Save the X509 certificate as PEM and writes it to the memory bio
    if (PEM_write_bio_X509(tmp_bio, server_certificate) != 1) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not write to memory bio");
    }

    // Get the length and a pointer to the bio's memory data
    long server_certificate_len;
    char *server_certificate_ptr;
    if ((server_certificate_len =
             BIO_get_mem_data(tmp_bio, &server_certificate_ptr)) <= 0) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not read from memory bio");
    }

    // Check if the size of the certificate is less than the maximum size of a
    // packet field
    if (server_certificate_len > FLEN_MAX) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors("Server's certificate length is bigger than the maximum "
                      "field's length");
    }

    // Actually send the certificate
    auto send_server_certificate_result = send_field<char>(
        socket, (flen)server_certificate_len, server_certificate_ptr);

    // and check the result
    if (send_server_certificate_result.is_error) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        BIO_free(tmp_bio);
        EVP_PKEY_free(client_half_key);
        handle_errors(send_server_certificate_result.error);
    }
    BIO_reset(tmp_bio);

    // Sign {g^x, g^y, C} with server's private key and send it

    // Init the signing context
    EVP_MD_CTX *server_signature_ctx;
    if ((server_signature_ctx = EVP_MD_CTX_new()) == nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        handle_errors("Could not allocate signing context");
    }
    EVP_SignInit(server_signature_ctx, get_hash_type());

    // Update the context with the data that has to be signed
    int err = 0;
    err |= EVP_SignUpdate(server_signature_ctx, client_half_key_pem,
                          client_half_key_len);
    err |= EVP_SignUpdate(server_signature_ctx, server_half_key_pem,
                          server_half_key_len);
    err |= EVP_SignUpdate(server_signature_ctx, username, username_len);

    if (err != 1) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        EVP_MD_CTX_free(server_signature_ctx);
        handle_errors("Could not sign correctly (update)");
    }

    // Open the server's private key file
    FILE *server_private_key_fp;
    if ((server_private_key_fp = fopen("certificates/server.key", "r")) ==
        nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        EVP_MD_CTX_free(server_signature_ctx);
        handle_errors("Could not open server's private key");
    }

    // And read the key from it
    EVP_PKEY *server_private_key;
    if ((server_private_key = PEM_read_PrivateKey(
             server_private_key_fp, nullptr, 0, nullptr)) == nullptr) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        EVP_MD_CTX_free(server_signature_ctx);
        fclose(server_private_key_fp);
        handle_errors("Could not read server's private key");
    }
    fclose(server_private_key_fp);

    unsigned char *server_signature =
        new unsigned char[get_signature_max_length(server_private_key)];
    unsigned int server_signature_len;

    if (EVP_SignFinal(server_signature_ctx, server_signature,
                      &server_signature_len, server_private_key) != 1) {
        free_user_keys(user_keys);
        delete[] username;
        delete[] client_half_key_pem;
        delete[] server_half_key_pem;
        delete[] server_signature;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        EVP_MD_CTX_free(server_signature_ctx);
        handle_errors("Could not sign correctly (final)");
    }

    EVP_MD_CTX_free(server_signature_ctx);
    delete[] server_half_key_pem;
    delete[] client_half_key_pem;

    if (server_signature_len > FLEN_MAX) {
        delete[] username;
        delete[] server_signature;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        handle_errors(
            "Server signature is bigger than the max packet field length");
    }

    auto send_server_signature_result =
        send_field(socket, (flen)server_signature_len, server_signature);
    if (send_server_signature_result.is_error) {
        delete[] username;
        delete[] server_signature;
        username = nullptr;
        EVP_PKEY_free(client_half_key);
        handle_errors(send_server_signature_result.error);
    }

    // ---------------------------------------------------------------------- //
    // -------------------- Client's response to Server --------------------- //
    // ---------------------------------------------------------------------- //

    // Receive client signature and check it

    // Compute shared secret and derive symmetric key
    auto key_res =
        kdf((unsigned char *)"placeholder", sizeof("placeholder"), key_len);
    if (key_res.is_error) {
        // TODO
        handle_errors(key_res.error);
    }
    return {username, key_res.result};
}
