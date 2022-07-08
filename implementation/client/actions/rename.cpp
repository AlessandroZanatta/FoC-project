#include "../../common/errors.h"
#include "../../common/seq.h"
#include "../../common/types.h"
#include "../../common/utils.h"
#include <openssl/evp.h>
#include <sys/socket.h>

void rename(int sock, unsigned char *key) {

    // all the filenames must have same size
    unsigned char *f_old = new unsigned char[FNAME_MAX_LEN];
    unsigned char *f_new = new unsigned char[FNAME_MAX_LEN];

    // get f_old
    cout << "File to rename: ";
    string input;
    getline(cin, input);
    f_old = string_to_uchar(input);

    // get f_new
    cout << "New name: ";
    getline(cin, input);
    f_new = string_to_uchar(input);

    // Generate iv for message
    auto iv_res = gen_iv();
    if (iv_res.is_error) {
        delete[] f_old;
        delete[] f_new;
        handle_errors(iv_res.error);
    }
    auto iv = iv_res.result;

    // Send logout request plaintext part
    auto send_packet_header_res =
        send_header(sock, RenameReq, seq_num, iv, get_iv_len());
    if (send_packet_header_res.is_error) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        handle_errors(send_packet_header_res.error);
    }

    // Initialize encryption context
    EVP_CIPHER_CTX *ctx;
    int len = 0;
    int ct_len;
    if ((ctx = EVP_CIPHER_CTX_new()) == nullptr) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        handle_errors("Could not encrypt message (alloc)");
    }

    if (EVP_EncryptInit(ctx, get_symmetric_cipher(), key, iv) != 1) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    int err = 0;
    unsigned char header = mtype_to_uc(RenameReq);
    err |=
        EVP_EncryptUpdate(ctx, nullptr, &len, &header, sizeof(unsigned char));
    err |=
        EVP_EncryptUpdate(ctx, nullptr, &len, seqnum_to_uc(), sizeof(seqnum));
    if (err != 1) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    // Actual encryption
    // First encrypt 128 bytes for f_old
    unsigned char *ct = new unsigned char[FNAME_MAX_LEN * 2 + get_block_size()];
    if (EVP_EncryptUpdate(ctx, ct, &len, f_old, FNAME_MAX_LEN) != 1) {
        delete[] f_old;
        delete[] f_new;
        delete[] iv;
        delete[] ct;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    ct_len = len;

    // Then encrypt 128 bytes for f_new
    if (EVP_EncryptUpdate(ctx, ct, &len, f_new, FNAME_MAX_LEN) != 1) {
        delete[] f_old;
        delete[] f_new;
        delete[] iv;
        delete[] ct;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    ct_len += len;

    // Finalize encryption
    if (EVP_EncryptFinal(ctx, ct + len, &len) != 1) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        delete[] ct;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    ct_len += len;

    unsigned char *tag = new unsigned char[TAG_LEN];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, tag) != 1) {
        delete[] iv;
        delete[] f_old;
        delete[] f_new;
        delete[] ct;
        delete[] tag;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    delete[] iv;
    EVP_CIPHER_CTX_free(ctx);

    // Send ciphertext
    auto ct_send_res = send_field(sock, (flen)ct_len, ct);
    if (ct_send_res.is_error) {
        delete[] ct;
        delete[] tag;
        delete[] f_old;
        delete[] f_new;
        handle_errors(ct_send_res.error);
    }
    delete[] ct;

    auto tag_send_res = send_field(sock, (flen)TAG_LEN, tag);
    if (tag_send_res.is_error) {
        delete[] tag;
        delete[] f_old;
        delete[] f_new;
        handle_errors(tag_send_res.error);
    }
    delete[] tag;
    delete[] f_old;
    delete[] f_new;

    inc_seqnum();

    //------------------Wait server response------------------
}