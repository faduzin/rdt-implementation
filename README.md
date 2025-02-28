# Implementação e Análise de Um Protocolo RDT Modificado Em Uma Rede de Computadores

**Autores:**  
- Éric F. C. Yoshida  
- Henrique C. Garcia  
- Lucas P. F. de Mattos  

**Instituto de Ciência e Tecnologia – Universidade Federal de São Paulo (UNIFESP)**  
São José dos Campos – SP – Brasil  
E-mails: eric.fadul@unifesp.br, henrique.garcia@unifesp.br, lucas.praxedes@unifesp.br

---

## Abstract

**English:**  
This document provides an overview of the RDT (Reliable Data Transfer) protocol and its various versions, highlighting the improvements implemented based on RDT 2.2. It describes essential mechanisms such as timeout and dynamic timeout, as well as flow control strategies, including transmission windows in both Go-Back-N and dynamic versions. The work also covers specific functionalities like binary file transfer initiated by a metadata packet (`PKT_START`), connection termination via a Fin-Ack-Fin-Ack handshake, and the fast retransmit technique to expedite packet loss recovery. Additionally, tests were conducted using the CORE emulator to simulate real-world scenarios with network losses and variations, demonstrating the robustness and effectiveness of the implemented mechanisms in ensuring reliable data transfer.

**Português:**  
Este documento apresenta uma visão geral do protocolo RDT (Reliable Data Transfer) e suas diversas versões, enfatizando as melhorias implementadas a partir do RDT 2.2. Nele, são descritos mecanismos essenciais, como os de timeout e timeout dinâmico, bem como estratégias de controle de fluxo, incluindo janelas de transmissão no modelo Go-Back-N e sua versão dinâmica. Também são abordadas funcionalidades específicas, como a transmissão de arquivos binários iniciada por um pacote de metadados (`PKT_START`), a finalização de conexão por meio de um handshake Fin-Ack-Fin-Ack e a técnica de fast retransmit para acelerar a recuperação de pacotes perdidos. Adicionalmente, foram realizados testes utilizando o CORE emulator para simular cenários reais com perdas e variações na rede, evidenciando a robustez e a eficácia dos mecanismos implementados na garantia de uma transferência confiável de dados.

---

## Introdução

O protocolo RDT (Reliable Data Transfer) foi concebido para assegurar a transmissão confiável de dados entre remetente e receptor, mesmo em ambientes onde a camada de rede não oferece garantias de confiabilidade. Inspirado nos trabalhos de Kurose e Ross, o RDT evoluiu por diversas versões, adaptando mecanismos para lidar com erros, perdas e duplicações de pacotes. Enquanto as primeiras versões assumiam um canal ideal sem falhas, versões subsequentes introduziram a detecção de erros via ACKs e NAKs, controle de fluxo e técnicas para gerenciar perdas de pacotes – culminando no rdt 3.0.

Nossa implementação foi baseada no RDT 2.2, permitindo a incorporação de funcionalidades específicas que ampliam a robustez e a eficiência da transferência de dados. Entre essas funcionalidades estão:
- **Timeout (estático e dinâmico)**
- **Janelas de transmissão (Go-Back-N e dinâmica)**
- **Transmissão de arquivos binários iniciada por pacote de metadados (`PKT_START`)**
- **Finalização de conexão via handshake Fin-Ack-Fin-Ack**
- **Fast Retransmit**  

Os testes utilizando o CORE emulator demonstraram a eficácia dos mecanismos implementados mesmo em cenários com perdas e variações na rede.

---

## O que é RDT e Suas Versões

**RDT (Reliable Data Transfer)** é um protocolo projetado para garantir a transferência confiável de dados sobre canais que, por si só, não oferecem tal garantia. Seguindo a abordagem de Kurose e Ross, temos:

- **rdt 1.0 – Canal Ideal:**  
  Assume que não há perdas ou erros na transmissão; não são necessárias verificações ou retransmissões.

- **rdt 2.0 – Detecção de Erros com ACK e NAK:**  
  Introduz o uso de _checksums_ para detectar corrupção de dados. O receptor envia um ACK para pacotes corretos ou um NAK para pacotes corrompidos, solicitando retransmissão.

- **rdt 2.1 – Simplificação do Controle de Erros:**  
  Elimina a necessidade de NAKs. Em vez disso, o receptor reenvia o último ACK válido quando detecta um erro, simplificando a lógica e evitando ambiguidades.

- **rdt 2.2 – Refinamento e Base para Implementações:**  
  Mantém os mecanismos anteriores com melhorias na eficiência e controle de pacotes duplicados. **Esta foi a base utilizada em nossa implementação.**

- **rdt 3.0 – Tratamento de Perdas de Pacotes:**  
  Adiciona temporizadores para retransmitir pacotes quando ACKs não são recebidos, permitindo operar mesmo em canais com perdas.

---

## Implementações a Partir do RDT 2.2 Base

### Timeout
- **O que:**  
  Mecanismo de temporização para detectar a não chegada de ACKs, identificando perdas de pacotes.
- **Por que:**  
  Essencial para retransmitir pacotes não confirmados em redes sujeitas a atrasos ou perdas.
- **Como:**  
  Para cada pacote enviado, um temporizador é iniciado. Se o ACK não chegar dentro do tempo estipulado, o pacote é retransmitido.
- **Vantagens:**  
  - Garante a recuperação de pacotes perdidos.  
  - Melhora a confiabilidade da transmissão.
- **Desvantagens:**  
  - Pode ocasionar retransmissões desnecessárias em redes com variações de delay.

### Timeout Dinâmico com Flag de Ativação
- **O que:**  
  Versão aprimorada onde o valor do timeout é ajustado dinamicamente conforme as condições da rede, podendo ser ativado ou desativado via flag.
- **Por que:**  
  Permite melhor adaptação às variações de delay, evitando retransmissões prematuras ou atrasos.
- **Como:**  
  Utiliza métricas como o round-trip time para recalcular o timeout continuamente.
- **Vantagens:**  
  - Adapta-se às condições reais da rede.  
  - Reduz retransmissões desnecessárias.
- **Desvantagens:**  
  - Aumenta a complexidade do sistema e pode resultar em valores imprecisos se os parâmetros não forem bem calibrados.

### Janela de Transmissão Go-Back-N
- **O que:**  
  Permite o envio de vários pacotes consecutivos sem esperar o ACK de cada um; em caso de erro, todos os pacotes subsequentes ao pacote com erro são retransmitidos.
- **Por que:**  
  Simplifica o controle de fluxo e a gestão de retransmissões.
- **Como:**  
  Um contador de sequência controla os pacotes enviados; em caso de timeout ou erro, a retransmissão é iniciada a partir do pacote problemático.
- **Vantagens:**  
  - Simplifica o controle do fluxo de dados.  
  - Permite maior utilização do canal.
- **Desvantagens:**  
  - Pode resultar em retransmissões excessivas, mesmo para pacotes já recebidos corretamente.

### Janela de Transmissão Dinâmica com Flag de Ativação
- **O que:**  
  O tamanho da janela de transmissão é ajustado dinamicamente com base nas condições da rede, podendo ser ativado via flag.
- **Por que:**  
  Otimiza a taxa de transferência adaptando o número de pacotes enviados simultaneamente conforme o desempenho da rede.
- **Como:**  
  Monitora métricas como o round-trip time e a taxa de erros para recalcular o tamanho da janela.
- **Vantagens:**  
  - Otimiza a utilização do canal.  
  - Aumenta a robustez do protocolo.
- **Desvantagens:**  
  - A complexidade do algoritmo pode causar instabilidades se não for ajustado corretamente.

### Transmissão de Arquivo Binário com `PKT_START`
- **O que:**  
  Mecanismo específico para a transmissão de arquivos binários, iniciada por um pacote especial chamado `PKT_START`, que contém o nome do arquivo e metadados.
- **Por que:**  
  Diferencia a transferência de arquivos de transmissões convencionais, permitindo ao receptor preparar-se para a recepção e processamento dos dados.
- **Como:**  
  O remetente envia um pacote `PKT_START` seguido pela sequência de pacotes com os dados do arquivo.
- **Vantagens:**  
  - Organiza e identifica os dados transmitidos.  
  - Permite procedimentos específicos de recepção e armazenamento.
- **Desvantagens:**  
  - Introduz overhead devido à transmissão de pacotes de controle.  
  - Requer lógica adicional para o tratamento dos metadados.

### Finalização de Conexão com 3 Way-Handshake
- **O que:**  
  Processo de encerramento ordenado da conexão utilizando um handshake composto por pacotes `fin` e `ack`.
- **Por que:**  
  Garante que ambas as partes reconheçam o fim da comunicação, evitando perda de pacotes pendentes.
- **Como:**  
  Inicia com o envio de um pacote com a flag `fin`, seguido de um `ack` do receptor, que por sua vez envia seu próprio `fin` finalizado com outro `ack`.
- **Vantagens:**  
  - Assegura o encerramento sincronizado da comunicação.  
  - Minimiza riscos de perda de dados.
- **Desvantagens:**  
  - Pode adicionar latência ao encerramento.  
  - Vulnerável a falhas se algum pacote de finalização ou confirmação for perdido.

### Fast Retransmit
- **O que:**  
  Técnica que acelera a recuperação de pacotes perdidos sem esperar pelo timeout tradicional.
- **Por que:**  
  Reduz a latência na recuperação de pacotes perdidos, contribuindo para uma transmissão mais eficiente.
- **Como:**  
  O remetente monitora os ACKs; ao detectar três ACKs duplicados para o mesmo número de sequência, retransmite imediatamente o pacote correspondente.
- **Vantagens:**  
  - Reduz o tempo de recuperação de pacotes.  
  - Melhora a continuidade da transmissão.
- **Desvantagens:**  
  - Aumenta a complexidade do protocolo.  
  - Pode causar retransmissões desnecessárias se a detecção de ACKs duplicados não for bem calibrada.

### Observações sobre as Flags de Ativação

Para as funcionalidades dinâmicas (timeout dinâmico e janela de transmissão dinâmica), foram implementadas flags de ativação. Essas flags permitem testar e validar cada funcionalidade de forma isolada, facilitando ajustes e comparações sem interferência entre os mecanismos.

---

## Validação da Implementação Usando o CORE Emulator

A validação foi realizada em uma topologia de rede simulada pelo **CORE Emulator**, permitindo a análise do comportamento do protocolo em condições reais de perda, corrupção de pacotes e congestionamento.

### Simulação do RDT

Foram executados processos de "Cliente" e "Servidor" nos nós n6 e n8, respectivamente. Durante as simulações, foi enviada a mesma transferência de arquivo binário em diversas configurações, dentre as quais:

- **Figuras 2 a 7:**  
  Temporizador e janela configurados de forma dinâmica.
- **Figura 8:**  
  Temporizador e janela estáticos.
- **Figura 9:**  
  Janela estática com temporizador dinâmico.

As configurações de rede testadas incluíram variações de Upstream, Downstream e percentuais de perda, tais como:
- Upstream e Downstream com 10 Kbps e 3% de perda.
- Upstream com 50 Kbps e Downstream com 10 Kbps ou 2 Kbps e 3% de perda.
- Upstream com 5 Kbps e Downstream com 50 Kbps e 3% de perda.
- Configurações onde ambos os parâmetros (Upstream e Downstream) eram de 50 Kbps com 3% de perda.

### Análise dos Testes

- **Figura 2:**  
  Taxas limitadas (10 Kbps) evidenciaram impacto da perda de pacotes, utilizado para verificação de integridade do arquivo.
- **Figura 3:**  
  Aumento da taxa de Upstream melhorou a eficiência do envio.
- **Figura 4:**  
  Downstream reduzido para 2 Kbps manteve boa vazão, embora com leve atraso nos ACKs.
- **Figura 5:**  
  Gargalo significativo com Upstream de 5 Kbps, aumentando a latência e reduzindo a vazão.
- **Figuras 6 e 7:**  
  Equilíbrio adequado com ambos os parâmetros em 50 Kbps, resultando em transmissão fluida.
- **Figura 8:**  
  Configuração estática resultou em menor eficiência em redes com variações.
- **Figura 9:**  
  Janela estática (com temporizador dinâmico) adaptou-se melhor que o cenário totalmente estático, mas subutilizou a banda disponível.

Os testes demonstraram que o principal gargalo era o Upstream e que a configuração dinâmica (tanto de timeout quanto de janela) proporcionou melhor adaptação às condições reais da rede.

---

## Contribuições dos Integrantes

- **Éric Fadul Cunha Yoshida:**  
  Implementação do código e testes do protocolo, além da escrita das respostas e conclusão do relatório.
- **Henrique Campanha Garcia:**  
  Auxílio na escrita e contextualização do relatório, descrição das figuras e formatação do trabalho.
- **Lucas Praxedes Fischer de Mattos:**  
  Resolução de erros na implementação final, testes com temporizador e janelas dinâmicas, análise das simulações e escrita dos testes.

A colaboração de todos foi fundamental para a organização e execução do projeto, garantindo clareza e qualidade na apresentação.

---

## Conclusão

Neste projeto foi apresentada uma implementação do protocolo RDT baseada na versão 2.2, com diversas melhorias, tais como:
- Mecanismos de timeout (estático e dinâmico).
- Janelas de transmissão (Go-Back-N e dinâmica).
- Transferência de arquivos binários iniciada por `PKT_START`.
- Finalização de conexão via handshake (Fin-Ack-Fin-Ack).
- Técnica de Fast Retransmit.

Os testes realizados com o CORE Emulator comprovaram a robustez e confiabilidade da solução, mesmo em cenários adversos. Apesar dos resultados positivos, ressalta-se que o uso do mecanismo Go-Back-N pode ser ineficiente em redes de alta latência ou com elevada taxa de erros, sugerindo que futuras melhorias poderiam incluir estratégias de retransmissão seletiva.

---

## Referências

- Kurose, J. F., & Ross, K. W. (2017). *Computer Networking: A Top-Down Approach* (7ª ed.). Pearson.

---

## Lista de Figuras

1. **Topologia de Conexão da Simulação**  
   ![Topologia de conexão da Simulação](<assets/Topologia_RDT_Imagem.png>)
2. **Cliente (n6) e Servidor (n8) – 10 Kbps Up/Down, 3% de perda**  
   ![RDT File Send](<assets/RDT_FileSend.png>)
3. **Upload – 50 Kbps Up, 10 Kbps Down, 3% de perda**  
   ![Upload 50u/10d](<assets/50u_10d_3loss.png>)
4. **Upload – 50 Kbps Up, 2 Kbps Down, 3% de perda**  
   ![Upload 50u/2d](<assets/50u_2d_3loss.png>)
5. **Upload – 5 Kbps Up, 50 Kbps Down, 3% de perda**  
   ![Upload 5u/50d](<assets/5u_50d_3loss.png>)
6. **Upload – 50 Kbps Up/Down, 3% de perda [Parte 1]**  
   ![Upload 50u/50d Parte 1](<assets/50u_50d_3loss-pt1.png>)
7. **Upload – 50 Kbps Up/Down, 3% de perda [Parte 2]**  
   ![Upload 50u/50d Parte 2](<assets/50u_50d_3loss-pt2.png>)
8. **Upload – 50 Kbps Up/Down, 3% de perda (Tempo e Janela estáticos)**  
   ![Upload 50u/50d estático](<assets/50u_50d_3loss-static.png>)
9. **Upload – 50 Kbps Up/Down, 3% de perda (Janela estática)**  
   ![Upload 50u/50d Janela estática](<assets/50u_50d_3loss-Wstatic.png>)

---

Esta implementação e análise demonstram a robustez do protocolo RDT modificado, sendo uma base sólida para aplicações que exijam transferência confiável de dados em ambientes de rede adversos.

---

Espero que este README seja útil para documentar o projeto no repositório. Caso haja necessidade de ajustes ou complementos, sinta-se à vontade para modificar conforme a necessidade do projeto.
