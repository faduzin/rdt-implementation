#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {  // Verifica se o número de argumentos está correto
        fprintf(stderr, "Uso: %s <porta_de_escuta>\n", argv[0]); // Exibe mensagem de uso
        exit(EXIT_FAILURE); // Encerra o programa com falha
    }
    
    int listen_port = atoi(argv[1]); // Porta de escuta
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Cria o socket
    if (sockfd < 0) { // Verifica erros
        perror("server: socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr; // Estrutura para o endereço do servidor
    memset(&server_addr, 0, sizeof(server_addr)); // Zera a estrutura
    server_addr.sin_family = AF_INET; // Define a família de endereços
    server_addr.sin_port = htons(listen_port); // Define a porta de escuta
    server_addr.sin_addr.s_addr = INADDR_ANY; // Aceita conexões de qualquer endereço
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) { // Associa o socket à porta
        perror("server: bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Escutando na porta %d.\n", listen_port); // Exibe mensagem de escuta
    
    
    if (rdt_recv_file(sockfd, "output.bin") < 0) { // Recebe o arquivo
        fprintf(stderr, "server: Erro ao receber o arquivo.\n"); // Exibe mensagem de erro
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Arquivo recebido com sucesso.\n"); // Exibe mensagem de sucesso
    close(sockfd); // Fecha o socket
    return 0;
}
