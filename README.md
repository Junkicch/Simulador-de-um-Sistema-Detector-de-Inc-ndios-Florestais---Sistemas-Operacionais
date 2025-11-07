# Simulador-de-um-Sistema-Detector-de-Inc-ndios-Florestais---Sistemas-Operacionais
Um simulador de detector de incêndios florestais usando pthreads.

-Para iniciar:
 -gcc incendio.c -o incendio
Ao rodar esse comando no terminal do computador, ira parecer um executavel na mesma pasta que você salvou esse script, ao abrir o executavel irá começar a simulação de uma floresta 30x30 represenada por ".", onde a cada 4 espaços tem um sensor representado por "T", acada um segundo é gerado um fogo na floresta representado por "@", quando os sensores identificam o incendio nos oito espaços ao redor do sensor, ele manda uma mensagem para os outros sensores, até chegar no sensor principal que são os sensores ao redor da floresta, os sensores principais mandam uma mensagem para o bombeiro, que apagam os fogos a cada dois segundos, deixando um "X" representando que o fogo foi apagado, existe o risco dos sensores serem queimados, assim desligando o sensor.
