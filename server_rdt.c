#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 2) { 
        fprintf(stderr, "Uso: %s <porta_de_escuta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int listen_port = atoi(argv[1]);
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("server: socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(listen_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("server: bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Escutando na porta %d.\n", listen_port);
    
    
    FILE *fp = NULL;
    pkt p, ack;
    struct sockaddr_in src;
    socklen_t addrlen;
    int ns, nr, rv;
    int totalBytes = 0;
    
    // Aguarda o PKT_START com os metadados do arquivo.
    addrlen = sizeof(struct sockaddr_in);
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)&src, &addrlen);
    if (nr < 0) {
        perror("rdt_recv_file: recvfrom(PKT_START)");
        return ERROR;
    }
    if (p.h.pkt_type != PKT_START) {
        fprintf(stderr, "rdt_recv_file: Esperado PKT_START, recebido outro tipo.\n");
        return ERROR;
    }
    // Extrai os metadados.
    file_meta meta;
    if (p.h.pkt_size - sizeof(hdr) < sizeof(file_meta)) {
        fprintf(stderr, "rdt_recv_file: Tamanho insuficiente para metadados.\n");
        return ERROR;
    }
    memcpy(&meta, p.msg, sizeof(file_meta));
    printf("rdt_recv_file: PKT_START recebido. Nome do arquivo: %s, Tamanho: %ld bytes.\n", meta.filename, meta.fileSize);
    // Envia ACK para o PKT_START.
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0)
        return ERROR;
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)&src, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_recv_file: sendto(PKT_START ACK)");
        return ERROR;
    }
    
    char filepath[100];
    strcpy(filepath, "receive/");
    strcat(filepath, meta.filename);
    // Abre o arquivo para escrita; utiliza o nome recebido nos metadados.
    fp = fopen(filepath, "wb");
    if (!fp) {
        perror("rdt_recv_file: fopen");
        return ERROR;
    }

    


    printf("server: Arquivo recebido com sucesso.\n");
    close(sockfd);
    return 0;
}
