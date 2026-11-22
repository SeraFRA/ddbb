#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct{
    uint32_t len;
    char frase[];
}stringa_t;



typedef struct 
{
    uint32_t hash;
    uint32_t key_len;
    uint32_t data_len;
    
    uint32_t ttl;
    char data[];
    
    
}dic_entry;

uint32_t calcolo_hash (stringa_t stringa){
    uint32_t i;
    uint32_t hash=0;

    // calcolo dell'hash sommando ogni valore char 8bit della stringa 
    for (i=0;i<stringa.len;i++){
        hash+=(uint32_t)stringa.frase[i];



    }
    return hash;
}

void strtostringa(char stringola[], stringa_t * punt){

    uint32_t len=0,i;
    for(i=0;stringola[i]!='\0';i++)len++;
    
    for(i=0;i<len;i++){punt->frase[i]=stringola[i];}

    punt->len = len;

}

void printstringhe(stringa_t *punt){
    int i;

    for(i=0;i<punt->len;i++) printf("%c",punt->frase[i]);
}


void scrivi_entry(dic_entry salvabile, char  * posto, uint32_t lunghezza ){
    memcpy(&salvabile.hash, posto,sizeof(uint32_t));
    posto+=4;
    memcpy(&salvabile.key_len, posto,sizeof(uint32_t));
    posto+=4;
    memcpy(&salvabile.data_len, posto,sizeof(uint32_t));
    posto+=4;
    memcpy(&salvabile.ttl, posto ,sizeof(uint32_t));
    posto+=4;
    memcpy(&salvabile.data, posto ,sizeof(lunghezza));



}

int salva_in_database(dic_entry salvabile, char  * iniziodb){
    uint32_t confronto=0,lunghezzadata=0,lunghezzakey=0,lunghezza=0, i;
    memcpy(&confronto,iniziodb,sizeof(uint32_t));
    
 
    while(salvabile.hash!=confronto){

        iniziodb+=4;
        memcpy(&lunghezzakey,iniziodb,sizeof(uint32_t));
        iniziodb+=4;
        memcpy(&lunghezzadata,iniziodb,sizeof(uint32_t));
        lunghezza=lunghezzadata+lunghezzakey;
        iniziodb+=4+lunghezza;
        memcpy(&confronto,iniziodb,sizeof(uint32_t));
    }

    scrivi_entry(salvabile, iniziodb, lunghezza);




}

void annulla_entry(dic_entry * entry, uint32_t size){
    memcpy(entry,0,8+size);
}

void annulla_dic(char * dictionary, uint32_t size){
    memcpy(dictionary,0,size);
}

void accoppia_key_value_e_crea_entry(stringa_t key, stringa_t data, dic_entry * entry, uint32_t ttl){
    uint32_t size=0,i;
    uint32_t hash =calcolo_hash(key);
    size=key.len+data.len;
    annulla_entry(entry,size);
    memcpy(entry,&hash,sizeof(uint32_t));
    entry+=4;
    memcpy(entry,&key.len, sizeof(uint32_t));
    entry+=4;
    memcpy(entry,&data.len, sizeof(uint32_t));
    entry+=4;
    memcpy(entry,&key.frase, key.len);
    entry+=key.len;
    memcpy(entry,&data.frase, data.len);
}


void printdic(char * dictionary){
    for (int i = 0; i < 500; i++)
    {
        printf("%c", *dictionary);
    }
    
}


int main (){
    stringa_t ciao,key;
    dic_entry entry;
    uint32_t ttl;
    char dictionary[500];
    
    annulla_dic(dictionary,500);
    strtostringa("ciao",&ciao);
    strtostringa("key",&key);
    accoppia_key_value_e_crea_entry(key,ciao,&entry,ttl);
    salva_in_database(entry,dictionary);
    printdic(dictionary);
 





   



    return 0;
}