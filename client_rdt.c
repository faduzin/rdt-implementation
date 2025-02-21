#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

#define MAX_DATA_SIZE 65536  // Limita o tamanho do bloco a 64KB

int main(int argc, char *argv[]) {
    if (argc != 4) { // Verifica se o número de argumentos está correto
        fprintf(stderr, "Uso: %s <server_ip> <server_port> <arquivo>\n", argv[0]); // Exibe mensagem de uso
        exit(EXIT_FAILURE); // Encerra o programa com falha
    }
    
    char *server_ip = argv[1]; // Endereço IP do servidor
    int server_port = atoi(argv[2]); // Porta do servidor
    char *filename = argv[3]; // Nome do arquivo a ser enviado
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Cria o socket
    if (sockfd < 0) { // Verifica erros
        perror("client: socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in dest_addr; // Estrutura para o endereço do servidor
    memset(&dest_addr, 0, sizeof(dest_addr)); // Zera a estrutura
    dest_addr.sin_family = AF_INET; // Define a família de endereços
    dest_addr.sin_port = htons(server_port); // Define a porta do servidor
    if (inet_pton(AF_INET, server_ip, &dest_addr.sin_addr) <= 0) { // Converte o endereço IP
        perror("client: inet_pton");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    FILE *fp = fopen(filename, "rb"); // Abre o arquivo para leitura
    if (!fp) {
        perror("client: fopen");
        return ERROR;
    }

    char buffer[MAX_DATA_SIZE]; // Buffer para armazenar os dados lidos
    size_t bytesRead; // Número de bytes lidos
    
    fseek(fp, 0, SEEK_END); // Move o ponteiro para o final do arquivo
    long fileSize = ftell(fp); // Obtém o tamanho do arquivo
    rewind(fp); // Retorna o ponteiro para o início do arquivo
    
    file_meta meta; // Estrutura para os metadados do arquivo
    memset(&meta, 0, sizeof(file_meta)); // Zera a estrutura
    strncpy(meta.filename, filename, sizeof(meta.filename)-1); // Copia o nome do arquivo
    meta.fileSize = fileSize; // Copia o tamanho do arquivo
    
    pkt startPkt; // Pacote de início
    
    if (make_pkt(&startPkt, PKT_START, 0, &meta, sizeof(file_meta)) < 0) { // Cria o pacote de início
        fclose(fp);
        return ERROR;
    }
    int ns = sendto(sockfd, &startPkt, startPkt.h.pkt_size, 0,
                    (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in)); // Envia o pacote de início
    if (ns < 0) { // Verifica erros
        perror("client: sendto(PKT_START)");
        fclose(fp);
        return ERROR;
    }

    printf("client: PKT_START enviado (seq %d). Nome: %s, Tamanho: %ld bytes.\n", 
           startPkt.h.pkt_seq, meta.filename, meta.fileSize); // Exibe informações

    // Loop para ler o bloco de dados e enviar ao servidor
    while ((bytesRead = fread(buffer, 1, MAX_DATA_SIZE, fp)) > 0) { // Lê o bloco de dados
        if(rdt_send(sockfd, buffer, bytesRead, &dest_addr) < 0) { // Envia o bloco de dados
            perror("client: rdt_send");
            fclose(fp);
            return ERROR;
        }
    }

    if(rdt_close(sockfd, &dest_addr, 0) < 0) { // Fecha a conexão
        perror("client: rdt_close");
        fclose(fp);
        return ERROR;
    }
    
    printf("client: Arquivo enviado com sucesso.\n");
    close(sockfd);
    return 0;
}
