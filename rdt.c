#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include "rdt.h"

// Configurações da janela e timeout estático padrão.
#define STATIC_WINDOW_SIZE 5
#define MAX_DYNAMIC_WINDOW 100
#define MIN_DYNAMIC_WINDOW 1
#define TIMEOUT_SEC        4
#define TIMEOUT_USEC       100000

// Variáveis globais para sequência (definidas como extern em rdt.h).
int biterror_inject = FALSE;
hseq_t _snd_seqnum = 1;
hseq_t _rcv_seqnum = 1;

// Variáveis globais para a janela de transmissão dinâmica.
int dynamic_window_enabled = TRUE;   // 0 = janela estática, 1 = janela dinâmica
int current_window_size = STATIC_WINDOW_SIZE;

// Variáveis globais para timeout: se dinâmico, serão ajustados.
int dynamic_timeout_enabled = TRUE;    // 0 = timeout estático, 1 = timeout dinâmico
int current_timeout_sec = TIMEOUT_SEC;
int current_timeout_usec = TIMEOUT_USEC;
const int MAX_TIMEOUT_SEC = 10;     // valor máximo de timeout
const int MIN_TIMEOUT_SEC = TIMEOUT_SEC; // valor mínimo de timeout

// Nova flag para ativar ou desativar o fast retransmit.
// 1 = fast retransmit ativado, 0 = fast retransmit desativado.
int fast_retransmit_enabled = FALSE;

// Função de checksum: calcula a soma de verificação do buffer.
unsigned short checksum(unsigned short *buf, int nbytes) {
    long sum = 0;
    while (nbytes > 1) {
        sum += *buf++;
        nbytes -= 2;
    }
    if (nbytes == 1) {
        sum += *(unsigned char *)buf;
    }
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)(~sum);
}

// Verifica se o pacote está corrompido.
int iscorrupted(pkt *pr) {
    pkt copy = *pr;
    unsigned short recv_csum = copy.h.csum;
    copy.h.csum = 0;
    unsigned short calc_csum = checksum((unsigned short *)&copy, copy.h.pkt_size);
    return (recv_csum != calc_csum);
}

// Cria um pacote com o header, copia o payload (se houver) e calcula o checksum.
int make_pkt(pkt *p, htype_t type, hseq_t seqnum, void *msg, int msg_len) {
    if (msg_len > MAX_MSG_LEN) {
        fprintf(stderr, "make_pkt: tamanho da mensagem %d excede MAX_MSG_LEN %d\n", msg_len, MAX_MSG_LEN);
        return ERROR;
    }
    p->h.pkt_size = sizeof(hdr);
    p->h.csum = 0;
    p->h.pkt_type = type;
    p->h.pkt_seq = seqnum;
    if (msg != NULL && msg_len > 0) {
        p->h.pkt_size += msg_len;
        memset(p->msg, 0, MAX_MSG_LEN);
        memcpy(p->msg, msg, msg_len);
    }
    p->h.csum = checksum((unsigned short *)p, p->h.pkt_size);
    return SUCCESS;
}

// Verifica se o pacote ACK recebido possui o número de sequência esperado.
int has_ackseq(pkt *p, hseq_t seqnum) {
    if (p->h.pkt_type != PKT_ACK || p->h.pkt_seq != seqnum)
        return FALSE;
    return TRUE;
}

// Função rdt_send: envia um buffer segmentado usando uma janela de transmissão.
// Se dynamic_window_enabled for 1, a janela é ajustada dinamicamente.
// O fast retransmit é acionado se a flag fast_retransmit_enabled estiver ativada.
int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst) {
    int chunk_size = MAX_MSG_LEN; // Tamanho máximo do payload
    int num_segments = (buf_len + chunk_size - 1) / chunk_size; // Número de segmentos
    double send_time[num_segments]; // Vetor para armazenar o tempo de envio de cada pacote
    double sample_rtt; // Variável para armazenar o SampleRTT
    double estimate_rtt = 0.100000; // Valor inicial do EstimateRTT
    double dev_rtt = 0.005000; // Valor inicial do DevRTT
    
    pkt *packets = malloc(num_segments * sizeof(pkt)); // Vetor de pacotes
    if (!packets) { // Verifica se a alocação foi bem-sucedida
        perror("rdt_send: malloc");
        return ERROR;
    }
    
    // Criação dos pacotes com os dados.
    for (int i = 0; i < num_segments; i++) {
        int offset = i * chunk_size; // Deslocamento no buffer
        int remaining = buf_len - offset; // Bytes restantes
        int seg_len = (remaining > chunk_size) ? chunk_size : remaining; // Tamanho do segmento
        if (make_pkt(&packets[i], PKT_DATA, _snd_seqnum + i, (char *)buf + offset, seg_len) < 0) { // Cria o pacote
            free(packets);
            return ERROR;
        }
    }
    
    // Ajusta a janela de transmissão: se dinâmica, usa current_window_size; caso contrário, STATIC_WINDOW_SIZE.
    current_window_size = dynamic_window_enabled ? current_window_size : STATIC_WINDOW_SIZE;
    
    int base = 0; // Base da janela
    int next_seq = 0; // Próximo número de sequência
    struct timeval timeout; // Timeout para select
    timeout.tv_sec = current_timeout_sec; // Timeout em segundos
    timeout.tv_usec = current_timeout_usec; // Timeout em microssegundos
    struct timeval send; // Variáveis para medir o tempo de envio
    struct timeval recv; // Variáveis para medir o tempo de recebimento
    fd_set readfds; // Conjunto de descritores de arquivo para select
    int ns, nr; // Número de bytes enviados e recebidos
    struct sockaddr_in ack_addr; // Endereço do ACK
    socklen_t addrlen; // Tamanho do endereço
    int dw_count = 5; // Contador para janela dinâmica
    // Variáveis para fast retransmission
    hseq_t last_ack_seq = 0; // Último número de sequência ACK recebido
    int dup_ack_count = 0; // Contador de ACKs duplicados
    hseq_t fastRetransmittedSeq = 0; // Número de sequência do pacote retransmitido
    
    while (base < num_segments) {
        // Envia os pacotes dentro da janela.
        while (next_seq < num_segments && next_seq < base + current_window_size) { // Enquanto houver espaço na janela
            pkt temp_pkt; // Pacote temporário
            memcpy(&temp_pkt, &packets[next_seq], sizeof(pkt)); // Copia o pacote
            
            // Injeção de erro (aplicada de forma randômica, se biterror_inject estiver ativo)
            if (biterror_inject) {
                if (rand() % 100 < 20) {  // 20% de chance
                    printf("rdt_send: Injetando erro no pacote seq %d (tentativa)\n", temp_pkt.h.pkt_seq);
                    memset(temp_pkt.msg, 0, MAX_MSG_LEN);
                    temp_pkt.h.csum = checksum((unsigned short *)&temp_pkt, temp_pkt.h.pkt_size);
                }
            }
            
            gettimeofday(&send,NULL); // Marca o tempo de envio

            ns = sendto(sockfd, &temp_pkt, temp_pkt.h.pkt_size, 0,
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in)); // Envia o pacote
            
            if (ns < 0) { // Verifica erros
                perror("rdt_send: sendto(PKT_DATA)");
                free(packets);
                return ERROR;
            }
            send_time[next_seq]=send.tv_sec+send.tv_usec; // Armazena o tempo de envio
            printf("rdt_send: Pacote enviado, seq %d\n", packets[next_seq].h.pkt_seq); // Exibe mensagem
            next_seq++; // Incrementa o número de sequência
        }
        
        FD_ZERO(&readfds); // Limpa o conjunto de descritores
        FD_SET(sockfd, &readfds); // Adiciona o socket ao conjunto
        
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda o recebimento de ACKs
        
        gettimeofday(&recv,NULL); // Marca o tempo de recebimento
        
        // Cálculo do TimeoutInterval
        if (dynamic_timeout_enabled) { // Se o timeout dinâmico estiver ativado
            sample_rtt = (recv.tv_sec - send.tv_sec) + (recv.tv_usec - send.tv_usec)/10e6; // Calcula o SampleRTT em segundos 
        	estimate_rtt = 0.875 * estimate_rtt + 0.125 * sample_rtt; // Calcula o EstimateRTT em segundos
        	
        	// Cálculo do módulo de Dev_RTT
        	if(sample_rtt-estimate_rtt>0) // Se SampleRTT > EstimateRTT
			dev_rtt = 0.75 * dev_rtt + 0.25 * (sample_rtt-estimate_rtt); // Calcula o DevRTT utilizando SampleRTT - EstimateRTT
 		else
 			dev_rtt = 0.75 * dev_rtt + 0.25 * (estimate_rtt-sample_rtt); // Calcula o DevRTT utilizando EstimateRTT - SampleRTT
        
        	// Atribuição
        	timeout.tv_sec = estimate_rtt + 4 * dev_rtt; // TimeoutInterval = EstimateRTT + 4 * DevRTT
       		timeout.tv_usec = (estimate_rtt + 4 * dev_rtt - timeout.tv_sec) * 1000000; // Converte para microssegundos
		
            if (timeout.tv_sec > MAX_TIMEOUT_SEC) // Limita o valor máximo do timeout
                timeout.tv_sec = MAX_TIMEOUT_SEC;
                
            printf("rdt_send: Timeout dinâmico alterado para %ld.%ld s\n", timeout.tv_sec, timeout.tv_usec/1000); // Exibe mensagem de alteração
        }else{
        	// Se for Timeout Estático
        	timeout.tv_sec = current_timeout_sec; // Timeout em segundos
    		timeout.tv_usec = current_timeout_usec; // Timeout em microssegundos
        }
        
        if (rv < 0) { // Verifica erros
            perror("rdt_send: select error");
            free(packets);
            return ERROR;
        } else if (rv == 0) { // Timeout
            printf("rdt_send: Timeout. Retransmitindo a partir do pacote seq %d\n", packets[base].h.pkt_seq);
            next_seq = base; // Volta para a base da janela
            
            // Cálculo da Janela Deslizante se Timeout
            if (dynamic_window_enabled){
            	// Subtrai o valor do ultimo pacote previsto e divide a janela por 2
            	dw_count -=current_window_size; // Subtrai o valor do ultimo pacote previsto
            	current_window_size /= 2; // Divide a janela por 2
            	
            	if (current_window_size < MIN_DYNAMIC_WINDOW) // Limita o valor mínimo da janela
                    current_window_size = MIN_DYNAMIC_WINDOW;
                
               	printf("rdt_send: Janela dinâmica diminuída para %d\n",current_window_size);
               	// Contador recomeça do pacote retransmitido até o fim da proxíma janela diminuída
                dw_count = packets[base].h.pkt_seq + current_window_size; // Contador recomeça do pacote retransmitido até o fim da proxíma janela diminuída
            }
            continue; // Reinicia o loop
          } else {
            pkt ack; // Pacote ACK
            addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
            nr = recvfrom(sockfd, &ack, sizeof(pkt), 0,
                          (struct sockaddr *)&ack_addr, &addrlen); // Recebe o ACK
            if (nr < 0) {
                perror("rdt_send: recvfrom(PKT_ACK)");
                free(packets);
                return ERROR;
            }
            if (iscorrupted(&ack) || ack.h.pkt_type != PKT_ACK) {
                printf("rdt_send: ACK corrompido ou inválido recebido.\n");
                continue;
            }
            
            // Se o fast retransmit estiver habilitado, processa os ACKs duplicados.
            if (fast_retransmit_enabled) { // Se o fast retransmit estiver ativado
                if (ack.h.pkt_seq == last_ack_seq) { // Se o ACK for duplicado
                    if (fastRetransmittedSeq != ack.h.pkt_seq) { // Se o pacote ainda não foi retransmitido
                        dup_ack_count++; // Incrementa o contador de ACKs duplicados
                        printf("rdt_send: ACK duplicado (%d) para o pacote seq %d\n", dup_ack_count, ack.h.pkt_seq); // Exibe mensagem de ACK duplicado
                        if (dup_ack_count >= 3) { // Se houver 3 ACKs duplicados
                            printf("rdt_send: Fast retransmission disparada para o pacote seq %d\n", packets[base].h.pkt_seq); // Exibe mensagem de fast retransmission
                            
                            next_seq = base; // Volta para a base da janela
                            fastRetransmittedSeq = ack.h.pkt_seq; // Marca o pacote retransmitido
                            dup_ack_count = 0; // Reseta o contador de ACKs duplicados
                            
                        // Cálculo da Janela Deslizante se Timeout
            		    if (dynamic_window_enabled){ // Se a janela dinâmica estiver ativada
            		  	// Subtrai o valor do ultimo pacote previsto e divide a janela por 2
            		        dw_count -=current_window_size; // Subtrai o valor do ultimo pacote previsto
            			    current_window_size /= 2; // Divide a janela por 2
		            	if (current_window_size < MIN_DYNAMIC_WINDOW) // Limita o valor mínimo da janela
                		    current_window_size = MIN_DYNAMIC_WINDOW;
                
               			printf("rdt_send: Janela dinâmica diminuída para %d\n",current_window_size); // Exibe mensagem
               			// Contador recomeça do pacote retransmitido até o fim da proxíma janela diminuída
                		dw_count = packets[base].h.pkt_seq + current_window_size; // Contador recomeça do pacote retransmitido até o fim da proxíma janela diminuída
            		    }
                        continue; // Reinicia o loop
                        }
                    }else {
                        printf("rdt_send: ACK duplicado para o mesmo pacote (seq %d) já retransmitido, ignorando.\n", ack.h.pkt_seq); // Exibe mensagem
                    }
                } else if (ack.h.pkt_seq > last_ack_seq) { // Se o ACK for maior que o último ACK recebido
                    last_ack_seq = ack.h.pkt_seq; // Atualiza o último ACK recebido
                    dup_ack_count = 0; // Reseta o contador de ACKs duplicados
                    fastRetransmittedSeq = 0; // Reseta o número de sequência do pacote retransmitido
                    int ack_index = ack.h.pkt_seq - _snd_seqnum; // Índice do ACK
                    if (ack_index >= base && ack_index < num_segments) { // Se o ACK estiver dentro da janela
                        printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq);
                        base = ack_index + 1; // Atualiza a base da janela
                    }                
                 }
            } else {
                // Se fast retransmit estiver desativado, ignoramos a contagem de ACKs duplicados.
                if (ack.h.pkt_seq > last_ack_seq) { // Se o ACK for maior que o último ACK recebido
                    last_ack_seq = ack.h.pkt_seq; // Atualiza o último ACK recebido
                    int ack_index = ack.h.pkt_seq - _snd_seqnum; // Índice do ACK
                    if (ack_index >= base && ack_index < num_segments) { // Se o ACK estiver dentro da janela
                        printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq); // Exibe mensagem
                        base = ack_index + 1; // Atualiza a base da janela
                    }
                }
            }
            
           // Cálculo da Janela Deslizante se tudo certo
           if (dynamic_window_enabled){ 
           	// Verifica se todos os ACKs da janela foram recebidos e a aumenta 
	   	if (ack.h.pkt_seq >= dw_count && current_window_size < MAX_DYNAMIC_WINDOW) { // Se todos os ACKs da janela foram recebidos e a janela não atingiu o máximo
		    current_window_size++; // Aumenta a janela
		    
		    if (current_window_size > MAX_DYNAMIC_WINDOW)
		        current_window_size = MAX_DYNAMIC_WINDOW;
	 	    printf("rdt_send: Janela dinâmica aumentada para %d\n", current_window_size); // Exibe mensagem
   		    dw_count +=current_window_size; // Incremento do contador com a nova janela
    		}
            }
        }
    }
    _snd_seqnum += num_segments; // Atualiza o número de sequência
    free(packets); // Libera a memória alocada
    return buf_len; // Retorna o tamanho do buffer
}


// Função rdt_close: envia um pacote FIN e aguarda o ACK correspondente.
int rdt_close(int sockfd, struct sockaddr_in *dst, int snd_seqnum) {
    int ns; // Número de bytes enviados
    pkt finPkt; // Pacote FIN
    if (make_pkt(&finPkt, PKT_FIN, snd_seqnum, NULL, 0) < 0) { // Cria o pacote FIN (sem payload)
        return ERROR;
    }
    
    ns = sendto(sockfd, &finPkt, finPkt.h.pkt_size, 0,
        (struct sockaddr *)dst, sizeof(struct sockaddr_in)); // Envia o pacote FIN
    if (ns < 0) { // Verifica erros
        perror("rdt_send_file: sendto(PKT_FIN)");
        return ERROR;
    }
    printf("rdt_send_file: Pacote FIN enviado (seq %d).\n", finPkt.h.pkt_seq); // Exibe mensagem de envio
    
    // Configura o timeout para aguardar o ACK
    struct timeval timeout = {current_timeout_sec, current_timeout_usec}; // Timeout
    fd_set readfds; // Conjunto de descritores de arquivo para select
    FD_ZERO(&readfds); // Limpa o conjunto
    FD_SET(sockfd, &readfds); // Adiciona o socket ao conjunto
    
    int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda o recebimento de ACK
    if (rv > 0) { // Se houve dados para leitura
        pkt ack; // Pacote ACK
        socklen_t addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
        ns = recvfrom(sockfd, &ack, sizeof(pkt), 0, NULL, &addrlen); // Recebe o ACK
        if (ns < 0) { // Verifica erros
            perror("rdt_close: recvfrom(PKT_FIN ACK)"); // Exibe mensagem de erro
            return ERROR;
        }
        if (ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == finPkt.h.pkt_seq) { // Se o ACK for válido
            printf("rdt_close: ACK do FIN recebido.\n"); // Exibe mensagem de sucesso
            return SUCCESS;
        } else {
            printf("rdt_close: ACK incorreto recebido.\n"); // Exibe mensagem de erro
            return ERROR;
        }
    } else {
        printf("rdt_close: Timeout aguardando ACK do FIN.\n"); // Exibe mensagem de timeout 
        return ERROR;
    }
}

int rdt_recv(int sockfd, void *buf, int buf_len, struct sockaddr_in *src) {
	pkt p, ack;
	int nr, ns;
	int addrlen;
	memset(&p, 0, sizeof(hdr));

        if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0)
                return ERROR;

rerecv:
	addrlen = sizeof(struct sockaddr_in);
	nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr*)src,
		(socklen_t *)&addrlen);
	if (nr < 0) {
		perror("recvfrom():");
		return ERROR;
	}
	if (iscorrupted(&p) || !has_dataseqnum(&p, _rcv_seqnum)) {
		printf("rdt_recv: iscorrupted || has_dataseqnum \n");
		// enviar ultimo ACK (_rcv_seqnum - 1)
		ns = sendto(sockfd, &ack, ack.h.pkt_size, 0,
			(struct sockaddr*)src, (socklen_t)sizeof(struct sockaddr_in));
		if (ns < 0) {
			perror("rdt_rcv: sendto(PKT_ACK - 1)");
			return ERROR;
		}
		goto rerecv;
	}
	int msg_size = p.h.pkt_size - sizeof(hdr);
	if (msg_size > buf_len) {
		printf("rdt_rcv(): tamanho insuficiente de buf (%d) para payload (%d).\n", 
			buf_len, msg_size);
		return ERROR;
	}
	memcpy(buf, p.msg, msg_size);
	// enviar ACK

	if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0)
                return ERROR;

	ns = sendto(sockfd, &ack, ack.h.pkt_size, 0,
                (struct sockaddr*)src, (socklen_t)sizeof(struct sockaddr_in));
	if (ns < 0) {
                perror("rdt_rcv: sendto(PKT_ACK)");
                return ERROR;
        }
	_rcv_seqnum++;
	return p.h.pkt_size - sizeof(hdr);
}
    

// Função rdt_recv_file: recebe um arquivo e grava no sistema de arquivos.
// O receptor espera inicialmente um PKT_START com metadados.
int rdt_recv_file(int sockfd, const char *filename) {
    FILE *fp = NULL; // Ponteiro para o arquivo
    pkt p, ack; // Pacotes
    struct sockaddr_in src; // Endereço do remetente
    socklen_t addrlen; // Tamanho do endereço
    fd_set readfds; // Conjunto de descritores de arquivo para select
    struct timeval timeout; // Timeout
    int ns, nr, rv; // Número de bytes enviados e recebidos, e valor de retorno
    int totalBytes = 0; // Número total de bytes recebidos
    
    // Aguarda o PKT_START com os metadados do arquivo.
    addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)&src, &addrlen); // Recebe o pacote
    if (nr < 0) { // Verifica erros
        perror("rdt_recv_file: recvfrom(PKT_START)"); // Exibe mensagem de erro
        return ERROR;
    }
    if (p.h.pkt_type != PKT_START) { // Verifica se o pacote é um PKT_START
        fprintf(stderr, "rdt_recv_file: Esperado PKT_START, recebido outro tipo.\n"); // Exibe mensagem de erro
        return ERROR;
    }
    // Extrai os metadados.
    file_meta meta; // Metadados do arquivo
    if (p.h.pkt_size - sizeof(hdr) < sizeof(file_meta)) { // Verifica se o tamanho do pacote é suficiente
        fprintf(stderr, "rdt_recv_file: Tamanho insuficiente para metadados.\n");
        return ERROR;
    }
    memcpy(&meta, p.msg, sizeof(file_meta)); // Copia os metadados
    printf("rdt_recv_file: PKT_START recebido. Nome do arquivo: %s, Tamanho: %ld bytes.\n", meta.filename, meta.fileSize); // Exibe mensagem de sucesso
    // Envia ACK para o PKT_START.
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) // Cria o pacote ACK
        return ERROR;
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
    if (ns < 0) { // Verifica erros
        perror("rdt_recv_file: sendto(PKT_START ACK)"); // Exibe mensagem de erro
        return ERROR;
    }
    
    char filepath[100]; // Caminho do arquivo
    strcpy(filepath, "receive/"); // Diretório de recebimento
    strcat(filepath, meta.filename); // Adiciona o nome do arquivo
    fp = fopen(filepath, "wb"); // Abre o arquivo para escrita
    if (!fp) {
        perror("rdt_recv_file: fopen");
        return ERROR;
    }
    
    // Recebe os pacotes de dados.
    while (1) {
        addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
        nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)&src, &addrlen); // Recebe o pacote
        if (nr < 0) { // Verifica erros
            perror("rdt_recv_file: recvfrom()");
            fclose(fp);
            return ERROR;
        }
        
        if (iscorrupted(&p)) { // Verifica se o pacote está corrompido
            printf("rdt_recv_file: Pacote corrompido, reenviando último ACK.\n"); // Exibe mensagem de erro
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) { // Cria o pacote ACK para o último pacote
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
            continue;
        }
        
        // Se for um pacote FIN, inicia o handshake de terminação.
        if (p.h.pkt_type == PKT_FIN) {
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) { // Cria o pacote ACK
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
            printf("rdt_recv_file: FIN recebido do cliente. ACK enviado para FIN.\n"); // Exibe mensagem de sucesso
            
            // Envia FIN do servidor.
            pkt serverFin; // Pacote FIN do servidor
            if (make_pkt(&serverFin, PKT_FIN, _snd_seqnum, NULL, 0) < 0) { // Cria o pacote FIN
                fclose(fp);
                return ERROR;
            }
            ns = sendto(sockfd, &serverFin, serverFin.h.pkt_size, 0,
                        (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o pacote FIN
            if (ns < 0) {
                perror("rdt_recv_file: sendto(PKT_FIN do servidor)");
                fclose(fp);
                return ERROR;
            }
            printf("rdt_recv_file: FIN enviado pelo servidor (seq %d).\n", serverFin.h.pkt_seq); // Exibe mensagem de sucesso
            
            // Aguarda ACK para o FIN do servidor.
            FD_ZERO(&readfds); // Limpa o conjunto de descritores
            FD_SET(sockfd, &readfds); // Adiciona o socket ao conjunto
            timeout.tv_sec = current_timeout_sec; // Timeout em segundos
            timeout.tv_usec = current_timeout_usec; // Timeout em microssegundos
            rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda o recebimento de ACK
            if (rv > 0) { // Se houve dados para leitura
                socklen_t ackAddrLen = sizeof(struct sockaddr_in); // Tamanho do endereço
                nr = recvfrom(sockfd, &ack, sizeof(pkt), 0, (struct sockaddr *)&src, &ackAddrLen); // Recebe o ACK
                if (nr > 0 && ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == serverFin.h.pkt_seq) { // Se o ACK for válido
                    printf("rdt_recv_file: ACK recebido para o FIN do servidor.\n"); // Exibe mensagem de sucesso
                }
            }
            break; // Encerra o loop
        }
        
        if (p.h.pkt_type == PKT_DATA && p.h.pkt_seq == _rcv_seqnum) { // Se for um pacote de dados e o número de sequência esperado
            int dataSize = p.h.pkt_size - sizeof(hdr); // Tamanho dos dados
            if (fwrite(p.msg, 1, dataSize, fp) != dataSize) { // Escreve os dados no arquivo
                perror("rdt_recv_file: fwrite");
                fclose(fp);
                return ERROR;
            }
            totalBytes += dataSize; // Atualiza o total de bytes recebidos
            printf("rdt_recv_file: Pacote recebido, seq %d (%d bytes).\n", p.h.pkt_seq, dataSize);  // Exibe mensagem de sucesso
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) { // Cria o pacote ACK
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
            _rcv_seqnum++;
        } else {
            printf("rdt_recv_file: Pacote fora de ordem (esperado seq %d).\n", _rcv_seqnum); // Exibe mensagem de erro (pacote fora de ordem)
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) { // Cria o pacote ACK para o último pacote
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
        }
    }
    
    fclose(fp);
    printf("rdt_recv_file: Transferência concluída. Total de bytes recebidos: %d\n", totalBytes); // Exibe mensagem de sucesso
    return totalBytes;
}