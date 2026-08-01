/*
 * Industrial pastry shop order management system
 *
 * Simulates, at discrete time steps, the full lifecycle of an order in a
 * pastry shop: recipes, ingredient stock (with expiration tracking), order
 * fulfillment and courier pickups.
 *
 * Original assignment: "Algoritmi e Principi dell'Informatica", Politecnico di Milano,
 * a.a. 2023/24 (David Ravelli).
 *
 * Design notes:
 *  - Recipes and stock are both stored in open hash tables (separate
 *    chaining) so lookup by name is O(1) on average.
 *  - Each ingredient's stock is a min-heap ordered by expiration date, so
 *    "use the batch closest to expiring first" and "drop everything already
 *    expired" are both cheap.
 *  - Pending orders are kept in a FIFO doubly linked list (arrival order
 *    matters when a restock unblocks them).
 *  - Orders ready for pickup sit in another FIFO list until the courier's
 *    time slot; at that point they're moved into a max-heap keyed on
 *    weight (ties broken by arrival time) to build the truck manifest.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM_HASH 5000          // number of buckets for both hash tables
#define MAX_SIZE 1000           // legacy cap, queues grow past this via linked list anyway
#define LEN_NOMI 20              // per-token name buffer size for scanf reads
#define CAPACITA_INIZIALE 100000 // initial capacity for the per-ingredient min-heap


// ---- Recipe book data structures ----------------------------------------

// One ingredient entry inside a recipe. idx1/idx2 cache the two hash values
// of the ingredient name so we can jump straight into the warehouse hash
// table later without recomputing/rehashing the string every time.
typedef struct Ingrediente {
    int quantita;
    unsigned int idx1; // primary hash -> warehouse bucket index
    unsigned int idx2;
    struct Ingrediente *next;
}Ingrediente;

typedef struct Ricetta{
    char *nome;
    struct Ingrediente *ingredienti;
    struct Ricetta *next;
}Ricetta;

typedef struct HashRicette{
    Ricetta **slot;
}HashRicette;

// ---- Warehouse data structures -------------------------------------------

// A single delivered batch of an ingredient.
typedef struct Lotto{
    int quantita;
    int scadenza;
}Lotto;

// Per-ingredient stock: a min-heap of batches ordered by expiration date,
// plus a running total so availability checks don't need to walk the heap.
typedef struct LottoMag{
    char *nome;
    int quantita_totale;
    int idx2;      // secondary hash, used to disambiguate collisions on idx1
    int capacita;
    int dim;
    Lotto** lotto;
    struct LottoMag *next;
}LottoMag;

typedef struct HashMagazzino{
    LottoMag **slot;
}HashMagazzino;



// ---- Order queues and courier structures ---------------------------------

typedef struct Ordine{
    Ricetta *ric;
    int quantita;
    int peso;
    int ist;    // time instant the order was placed (used for tie-breaks / FIFO order)
}Ordine;

// Max-heap used only at courier pickup time to sort loaded orders by weight
// (heaviest first, ties broken by earlier arrival).
typedef struct NodoHeapCorriere{
    Ordine **ordine;
    int capacita;
    int dim;
}NodoHeapCorriere;

// Generic doubly linked list node, reused for both the "waiting for stock"
// queue and the "ready for courier" queue below.
typedef struct NodoCoda{
    Ordine *ordine;
    struct NodoCoda *prev;
    struct NodoCoda *next;
}NodoCoda;

typedef struct Coda{
    NodoCoda *head;
    NodoCoda *tail;
    int size;
}Coda;


typedef struct NodoCodaCorriere{
    Ordine *ordine;
    struct NodoCodaCorriere *prev;
    struct NodoCodaCorriere *next;
}NodoCodaCorriere;

typedef struct CodaCorriere{
    NodoCodaCorriere *head;
    NodoCodaCorriere *tail;
    int size;
}CodaCorriere;

// ---- String hashing -------------------------------------------------------
// Two independent hash functions on the same name: the first picks the
// bucket, the second is stored alongside each entry as a cheap way to
// disambiguate two different names that happen to collide on the first hash
// (avoids a full strcmp against every entry in a crowded bucket).

unsigned int hashString(char input[]){
    unsigned int sum = 0;
    for (unsigned int i = 0; input[i] != '\0'; i++) {
        sum = sum * 37 + (unsigned char)input[i];
    }
    return sum % DIM_HASH;
}

unsigned int hashString2(char* str) {
    unsigned int hash = 7;
      while (*str) {
        hash = (hash * 33) ^ (unsigned char)*str++;
    }
    return hash % DIM_HASH;
}


// ---- Recipe book: creation, lookup, insertion, removal --------------------

void deallocaRicettario(HashRicette *hash){
    if(hash == NULL){
        return;
    }

    for(int i = 0; i < DIM_HASH; i++){
        Ricetta *ric = hash->slot[i];
        Ricetta *prev;
        while(ric != NULL){
            prev = ric;
            ric = ric->next;
            Ingrediente *ing = prev->ingredienti;
            while(ing != NULL){
                Ingrediente *prec = ing;
                ing = ing->next;
                free(prec);
            }

            free(prev->nome);
            free(prev);

        }
    }
    free(hash->slot);
    free(hash);
}

HashRicette *creaHashRicette(){
    HashRicette *hash = malloc(sizeof(HashRicette));

    if(hash == NULL){
        perror("Error: hash is null");
    }

    hash->slot = malloc(DIM_HASH * sizeof(Ricetta *));
    if(hash->slot == NULL){
        perror("Error: slot is null");
        free(hash);
    }

    for (int i = 0; i < DIM_HASH; i++){
        hash->slot[i] = NULL;
    }

    return hash;

}

// Builds an ingredient list node and prepends it to "lista" (caller owns
// the resulting head). Hash values are precomputed here once and reused
// for the lifetime of the ingredient.
Ingrediente *inserisiciIngrediente(Ingrediente *lista, char nome[], int quantita){
    unsigned int hash = hashString(nome);
    unsigned int hash2 = hashString2(nome);
    Ingrediente *ing = malloc(sizeof(Ingrediente));
    if(ing == NULL){
        perror("Errore ingredienti");
    }

    ing->quantita = quantita;
    ing->next = lista;
    ing->idx1 = hash;
    ing->idx2 = hash2;
    
    return ing;
}

void inserisciRicetta(HashRicette *hash, char nome[], Ingrediente *lista){
    unsigned int hfunc = hashString(nome);
    
    Ricetta *ricetta = malloc(sizeof(Ricetta));
    if(ricetta == NULL){
        perror("Errore ricett alloc");
    }
    ricetta->nome = malloc(strlen(nome) * sizeof(char) + 1);
    if(ricetta->nome == NULL){
        perror("Errore ricett nome alloc");
    }
    ricetta->ingredienti = NULL;
    ricetta->next = NULL;
    strcpy(ricetta->nome, nome);
    ricetta->ingredienti = lista;
    // insert at the head of the bucket's chain, O(1)
    ricetta->next = hash->slot[hfunc];
    hash->slot[hfunc] = ricetta;

}

Ricetta *trovaRicetta(HashRicette *hash, char nome[]){
    unsigned int slot = hashString(nome);
    Ricetta *testa = hash->slot[slot];

    while((testa != NULL) && (strcmp(nome, testa->nome))){
        testa = testa->next;
    }
    return testa;
}

void rimuoviRicetta(HashRicette *hash, char nome[]){
    unsigned int slot = hashString(nome);
    Ricetta *testa = hash->slot[slot];
    Ricetta *prec = NULL;

    while((testa != NULL) && (strcmp(nome, testa->nome))){
        prec = testa;
        testa = testa->next;
    }

    if(testa == NULL){
        printf("non presente\n");
        return;
    }
    
    if(prec == NULL){
        hash->slot[slot] = testa->next;
    }else{
        prec->next = testa->next;
    }

    Ingrediente *ing = testa->ingredienti;
    while(ing != NULL){
        Ingrediente *temp = ing;
        ing = ing->next;
        free(temp);
    }

    free(testa->nome);
    free(testa);

    printf("rimossa\n");

}

// ---- Warehouse: min-heap of batches per ingredient -------------------------
// Standard binary heap stored in a dynamic array; ordered on "scadenza"
// (expiration instant) so the batch closest to expiring is always at the
// root and gets consumed first.

void swapLotti(Lotto** a, Lotto** b){
    Lotto* temp = *a;
    *a = *b;
    *b = temp;
}

int padre(int i) { return (i-1)/2; }
int sinistro(int i) { return 2*i+1;}
int destro(int i ){ return 2*i+2;}

void creaMinHeap(LottoMag* lottoMag, int capacita){
    lottoMag->capacita = capacita;
    lottoMag->dim = 0;
    lottoMag->lotto = (Lotto**)malloc(capacita*sizeof(Lotto*));
}

void raddoppiaCap(LottoMag* heap){
    heap->capacita *= 2;
    heap->lotto = (Lotto**)realloc(heap->lotto, heap->capacita * sizeof(Lotto*));
}

// Sift-down from idx, restoring the min-heap property on "scadenza".
void minHeapify(LottoMag* heap, int idx){
    int min = idx;
    int sx = sinistro(idx);
    int dx = destro(idx);


    if((sx < heap->dim) && (heap->lotto[sx]->scadenza < heap->lotto[min]->scadenza)){
        min = sx;
    }

    if((dx < heap->dim) && (heap->lotto[dx]->scadenza < heap->lotto[min]->scadenza)){
        min = dx;
    }

    if(min != idx){
        swapLotti(&heap->lotto[min], &heap->lotto[idx]);
        minHeapify(heap, min);
    }
}

// Sift-up insertion: append at the end, then bubble up while the parent has
// a later expiration date.
void insMinHeap(LottoMag* heap, Lotto* lotto){
    if(heap == NULL){
        return;
    }
    if(heap->dim == heap->capacita){
        raddoppiaCap(heap);
    }
    int i = heap->dim++;
    heap->lotto[i] = lotto;

    while((i != 0) && (heap->lotto[padre(i)]->scadenza > heap->lotto[i]->scadenza)){
        swapLotti(&heap->lotto[i], &heap->lotto[padre(i)]);
        i = padre(i);
    }
}


// Pops the batch with the closest expiration date, keeping quantita_totale
// consistent with what's actually left in the heap.
Lotto* estraMin(LottoMag* lottoMag) {
    if (lottoMag == NULL || lottoMag->lotto == NULL || lottoMag->dim == 0) {
        return NULL;
    }

    Lotto* radice = lottoMag->lotto[0];
    
    lottoMag->dim--;
    if (lottoMag->dim > 0) {
        lottoMag->lotto[0] = lottoMag->lotto[lottoMag->dim];
        lottoMag->quantita_totale -= radice->quantita;
        minHeapify(lottoMag, 0);
    } else {
        lottoMag->quantita_totale = 0;  // heap just emptied
    }

    return radice;
}


// Drops every batch whose expiration is <= t. Since the heap is ordered by
// expiration date, we only ever need to look at the root.
void rimuoviScaduti(LottoMag *lottoMag, int t){
    while(lottoMag->dim > 0 && lottoMag->lotto[0]->scadenza <= t){
        Lotto* scad = estraMin(lottoMag);
        free(scad);
    } 
}

// Full lookup by name (used sparingly - once an ingredient's idx1/idx2 pair
// is known, trovaLottoIngMag below is the fast path).
LottoMag *trovaLottoMag(HashMagazzino *hash, char nome[]){
    unsigned int slot = hashString(nome);
    unsigned int hash2 = hashString2(nome);
    LottoMag *testa = hash->slot[slot];

    while((testa != NULL) && (strcmp(nome, testa->nome) && (hash2 != testa->idx2))){
        testa = testa->next;
    }
    return testa;
}

// Fast path lookup: jump directly into the bucket using the cached hash and
// disambiguate collisions with the secondary hash instead of a strcmp.
LottoMag *trovaLottoIngMag(HashMagazzino *hash, unsigned int idx1, unsigned int idx2){
    LottoMag *testa = hash->slot[idx1];

    while((testa != NULL) && (idx2 != testa->idx2)){
        testa = testa->next;
    }

    return testa;
}



// ---- Warehouse hash table management ---------------------------------------
HashMagazzino *creaHashMagazzino(){
    HashMagazzino *hash = malloc(sizeof(HashMagazzino));

    if(hash == NULL){
        perror("Error: hash is null");
    }

    hash->slot = malloc(DIM_HASH * sizeof(LottoMag *));
    if(hash->slot == NULL){
        perror("Error: slot is null");
        free(hash);
    }

    for (int i = 0; i < DIM_HASH; i++){
        hash->slot[i] = NULL;
    }

    return hash;

}

// Adds a new batch to the given ingredient's stock, creating the stock
// entry (and its heap) the first time this ingredient shows up. Expired
// batches are swept right away so quantita_totale never drifts.
void inserisciLottoMag(HashMagazzino *hash, char nome[], int quantita, int scadenza, int t){
    unsigned int hfunc = hashString(nome);
    unsigned int hfunc2 = hashString2(nome);

    LottoMag *lottoMag = trovaLottoMag(hash, nome);

    if(lottoMag == NULL){

        lottoMag = malloc(sizeof(LottoMag));
            
        lottoMag->nome = malloc(strlen(nome) * sizeof(char) + 1);
        if(lottoMag->nome == NULL){
            perror("Errore ricett nome alloc");
        }
        lottoMag->next = NULL;
        strcpy(lottoMag->nome, nome);
        lottoMag->next = hash->slot[hfunc];
        hash->slot[hfunc] = lottoMag;
        lottoMag->quantita_totale = 0;
        lottoMag->idx2 = hfunc2;
        creaMinHeap(lottoMag, CAPACITA_INIZIALE);
    }

    lottoMag->quantita_totale += quantita;

    Lotto *lotto = malloc(sizeof(Lotto));

    lotto->quantita = quantita;
    lotto->scadenza = scadenza;

    insMinHeap(lottoMag, lotto);

    rimuoviScaduti(lottoMag, t);
    

}

// ---- Courier queue (orders ready, waiting for the truck) -------------------
// Plain FIFO doubly linked list, kept in arrival order.

void inizializzaCodaCorriere(CodaCorriere* q){
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}

int isEmptyCorriere(CodaCorriere* q){
    return q->size == 0;
}

int isFullCorriere(CodaCorriere* q){
    return q->size == MAX_SIZE;
}

void enqueueCorriere(CodaCorriere* q, Ordine *x) {
    NodoCodaCorriere* nuovo = (NodoCodaCorriere*)malloc(sizeof(NodoCodaCorriere));
    if (nuovo == NULL) {
        perror("Errore di allocazione memoria");
    }
    nuovo->ordine = x;
    nuovo->next = NULL;
    nuovo->prev = q->tail;

    if (q->tail) {
        q->tail->next = nuovo;
    } else {
        q->head = nuovo;
    }
    q->tail = nuovo;
    q->size++;
}

Ordine *dequeueCorriere(CodaCorriere *q) {
    if (q->head == NULL) {
        return NULL;
    }

    NodoCodaCorriere* daRimuovere = q->head;
    Ordine* ordine = daRimuovere->ordine;

    q->head = daRimuovere->next;
    if (q->head) {
        q->head->prev = NULL;
    } else {
        q->tail = NULL;
    }

    q->size--;

    return ordine;
}

// Linear scan used only by rimuovi_ricetta, to check whether an order tied
// to this recipe is still sitting in the courier queue.
Ordine* trovaOrdineCorriere(CodaCorriere *q, char nome[]) {
    if (isEmptyCorriere(q)) {
        return NULL;
    }

    NodoCodaCorriere *curr = q->head;
    while (curr) {
        if (strcmp(curr->ordine->ric->nome, nome) == 0) {
            return curr->ordine;
        }
        curr = curr->next;
    }
    return NULL;
}

// ---- Courier max-heap (by weight, then by earlier arrival) -----------------
// Only populated at pickup time: orders that fit on the truck are moved
// here from the FIFO queue so we can print the manifest in the required
// "heaviest first" order without a full sort.

void swapOrdini(Ordine** a, Ordine** b){
    Ordine* temp = *a;
    *a = *b;
    *b = temp;
}
void raddoppiaCapCorriere(NodoHeapCorriere* heap){
    heap->capacita *= 2;
    heap->ordine = (Ordine**)realloc(heap->ordine, heap->capacita * sizeof(Ordine*));
}

NodoHeapCorriere* creaMaxHeapCorriere(int capacita){
    NodoHeapCorriere* heap = (NodoHeapCorriere*)malloc(sizeof(NodoHeapCorriere));
    heap->capacita = capacita;
    heap->dim = 0;
    heap->ordine = (Ordine**)malloc(capacita*sizeof(Ordine*));
    return heap;
}


// Ordering rule: heavier order wins; on a tie, the one placed earlier
// (smaller "ist") wins, matching the "chronological order on equal weight"
// requirement from the spec.
void maxHeapifyCorriere(NodoHeapCorriere* heap, int idx){
    int max = idx;
    int sx = sinistro(idx);
    int dx = destro(idx);

    if(sx < heap->dim && (heap->ordine[sx]->peso > heap->ordine[max]->peso || (heap->ordine[sx]->peso == heap->ordine[max]->peso && heap->ordine[sx]->ist < heap->ordine[max]->ist))){
        max = sx;
    }

    if(dx < heap->dim && (heap->ordine[dx]->peso > heap->ordine[max]->peso || (heap->ordine[dx]->peso == heap->ordine[max]->peso && heap->ordine[dx]->ist < heap->ordine[max]->ist))){
        max = dx;
    }

    if(max != idx){
        swapOrdini(&heap->ordine[max], &heap->ordine[idx]);
        maxHeapifyCorriere(heap, max);
    }
}

void insMaxHeapCorriere(NodoHeapCorriere* heap, Ordine* ordine){
    if(heap->dim == heap->capacita){
        raddoppiaCapCorriere(heap);
    }
    int i = heap->dim++;
    heap->ordine[i] = ordine;

    while((i != 0) && ((heap->ordine[padre(i)]->peso < heap->ordine[i]->peso) || ((heap->ordine[padre(i)]->peso == heap->ordine[i]->peso) && (heap->ordine[padre(i)]->ist > heap->ordine[i]->ist)))){
        swapOrdini(&heap->ordine[i], &heap->ordine[padre(i)]);
        i = padre(i);
    }

}

Ordine* estraiMaxCorriere(NodoHeapCorriere* heap){
    if(heap->dim == 0){
        return NULL;
    }

    Ordine* radice = heap->ordine[0];
    heap->ordine[0] = heap->ordine[--heap->dim];
    maxHeapifyCorriere(heap, 0);

    return radice;
}

// ---- Waiting queue (orders that couldn't be fulfilled yet) -----------------
// Same FIFO structure as the courier queue; orders here are retried, in
// arrival order, every time a restock comes in.

void inizializzaCoda(Coda* q){
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}

int isEmpty(Coda* q){
    return q->size == 0;
}

int isFull(Coda* q){
    return q->size == MAX_SIZE;
}

void enqueue(Coda* q, Ordine *x){

    NodoCoda* new = (NodoCoda*)malloc(sizeof(NodoCoda));
    new->ordine = x;
    new->next = NULL;

    if(q->tail == NULL){
        new->prev = NULL;
        q->head = new;
        q->tail = new;
    }else{
        new->prev = q->tail;
        q->tail->next = new;
        q->tail = new;
    }
    q->size++;
}

Ordine* dequeue(Coda* q) {
    if (q->head == NULL) {
        return NULL;
    }

    NodoCoda* daRimuovere = q->head;
    Ordine* ordine = daRimuovere->ordine;

    q->head = daRimuovere->next;
    if (q->head != NULL) {
        q->head->prev = NULL;
    } else {
        q->tail = NULL;
    }

    free(daRimuovere);
    q->size--;

    return ordine;
}

// Removes a specific order from the middle of the waiting list once it's
// been fulfilled, without having to rebuild the whole list.
void rimuoviOrdine(Coda* q, Ordine* ordine) {
    NodoCoda* corrente = q->head;

    while (corrente != NULL) {
        if (corrente->ordine == ordine) {
            if (corrente->prev != NULL) {
                corrente->prev->next = corrente->next;
            } else {
                q->head = corrente->next;
            }

            if (corrente->next != NULL) {
                corrente->next->prev = corrente->prev;
            } else {
                q->tail = corrente->prev;
            }

            free(corrente);
            q->size--;
            return;
        }
        corrente = corrente->next;
    }
}

// Linear scan used by rimuovi_ricetta to check for pending orders tied to
// a recipe before allowing its removal.
Ordine* trovaOrdineRic(Coda *q, char nome[]){
    if(isEmpty(q)){
        return NULL;
    }

    NodoCoda *curr = q->head;
    while(curr != NULL){
        Ordine *o = curr->ordine;
        if (o != NULL && strcmp(o->ric->nome, nome) == 0) {
            return o;
        }
        curr = curr->next;
    }
    return NULL;
}

// Distinguishes an ingredient/quantity token from the next command keyword
// while parsing a variable-length "aggiungi_ricetta" line.
int isCommand(const char *str) {
    return strcmp(str, "aggiungi_ricetta") == 0 || strcmp(str, "rimuovi_ricetta") == 0 ||
           strcmp(str, "rifornimento") == 0 || strcmp(str, "ordine") == 0;
}


int main(){
    int t = 0;
    int periodo;
    int capienza;
    
    // Core data structures for the whole simulation
    HashRicette *ricettario = creaHashRicette();
    if(ricettario == NULL){
        perror("Errore allocazione ricettario");
    }
    HashMagazzino *magazzino = creaHashMagazzino();
    if(magazzino == NULL){
        perror("Errore allocazione magazzino");
    }
    Coda *attesa = malloc(sizeof(Coda));
    inizializzaCoda(attesa);
    CodaCorriere *corriere = malloc(sizeof(CodaCorriere));
    inizializzaCodaCorriere(corriere); 
    NodoHeapCorriere *heap = creaMaxHeapCorriere(CAPACITA_INIZIALE);
    
    // First line: courier period and truck capacity
    if (scanf("%d %d", &periodo, &capienza) != 2) {
        perror("Errore lettura di periodo e capienza");
        return 1;
    }

    getchar();

    char comando[50];

    // Main command loop
    while(scanf("%s", comando) != EOF){

        // Courier pickup: triggered before processing the command at every
        // time instant that's a multiple of "periodo" (and t > 1, since the
        // very first tick has nothing ready yet).
        if(t>1 && ((t % periodo) == 0)){
            int qty = 0;
            int poss = 1;
            if (corriere->size != 0){
           
                // Load orders in arrival order until the next one would
                // exceed the truck's remaining capacity.
                while(poss && (corriere->size != 0)){
                    Ordine *o = corriere->head->ordine;
                    qty += o->peso;
                    if(qty >= capienza){
                        poss = 0;
                    }else{
                        o = dequeueCorriere(corriere);
                        insMaxHeapCorriere(heap, o);
                    }
                }
              

            Ordine *o = malloc(sizeof(Ordine));
            // Drain the max-heap to print the manifest heaviest-first
            // (ties broken by earlier arrival, handled inside the heap).
            do{
                o = estraiMaxCorriere(heap);
                if(o != NULL){
                    printf("%d %s %d\n", o->ist, o->ric->nome, o->quantita);
                }
                free(o);
            }while(heap->dim != 0);

            }else{
                printf("camioncino vuoto\n");
            }
       
        }

        if(comando[0] == 'a'){
        if(strcmp("aggiungi_ricetta", comando) == 0){
            char nome[LEN_NOMI];
            if (scanf("%s", nome) != 1) {
                perror("Errore durante la lettura del nome della ricetta");
                return 1;
            }

            if(trovaRicetta(ricettario, nome) != NULL){
                // Recipe already exists: consume and discard the rest of
                // the line instead of parsing it.
                if (scanf("%*[^\n]") == EOF){
                    break;
                }
                 if (scanf("%*c") == EOF){
                    break;
                }
                
                printf("ignorato\n");
            }else{
                // New recipe: read (ingredient, quantity) pairs until the
                // next token is a command keyword or the line ends.
                Ingrediente *ingr = NULL;

                char ing[LEN_NOMI];
                int qt;

                while((scanf("%s", ing) == 1) && (!isCommand(ing)) ){
                    if (scanf("%d", &qt) != 1) break;
                    ingr = inserisiciIngrediente(ingr, ing, qt);
                    char c;
                    if((c = getchar()) == '\n'){
                        break;
                    }
                }
                inserisciRicetta(ricettario, nome, ingr);
                printf("aggiunta\n");   
            }
            t++;
        }

        }else if(comando[0] == 'r'){
        if(strcmp("rimuovi_ricetta", comando) == 0){
                
                char nome[LEN_NOMI] = {0};

                if (scanf("%s", nome) != 1) {
                perror("Errore durante la lettura del nome della ricetta");
                return 1;
            }

                // Can't drop a recipe that still has shipments outstanding,
                // whether waiting for stock or already queued for the courier.
                if((trovaOrdineRic(attesa, nome) != NULL) || (trovaOrdineCorriere(corriere, nome))){
                    printf("ordini in sospeso\n");
                }else{
                rimuoviRicetta(ricettario, nome);
                } 
                t++;
        }

        if(strcmp("rifornimento", comando) == 0){
            char ing[LEN_NOMI];
            int qt;
            int sc;
            
            while(scanf("%s %d %d", ing, &qt, &sc) == 3){
                inserisciLottoMag(magazzino, ing, qt, sc, t);
                char c;
                if((c = getchar()) == '\n'){
                        break;
                    }
            }
            printf("rifornito\n");

            if(!isEmpty(attesa)){
                // A restock might unblock orders that are waiting. Walk the
                // waiting list in arrival order and try to fulfill each one
                // in turn, exactly as if it were a fresh order.
                NodoCoda *corrente = attesa->head;
                int i = 0;
                int lim = attesa->size;
                while(i < lim){
                    Ordine *o = corrente->ordine;
                    if(o != NULL){

                            Ricetta *ricetta = o->ric;
                            Ingrediente *ing = ricetta->ingredienti;
                            int possibile = 1;
                        int qt;
                        if(!o){
                            perror("Errore allocazione memoria");
                        }
                        int numero = o->quantita;

                        // Check availability first, without touching stock.
                        while(possibile && (ing != NULL)){
                            qt = ing->quantita * numero;
                            
                                LottoMag *lottoMag = trovaLottoIngMag(magazzino, ing->idx1, ing->idx2);
                                if(lottoMag != NULL){
                                        if(qt > lottoMag->quantita_totale){
                                            possibile = 0;
                                            break;
                                        }
                                }else{
                                    possibile = 0;
                                    break;
                                }
                            if(possibile){
                                ing = ing->next;
                            }
                        }
                        
                        if(possibile){
                            // Enough stock: consume it, expiring-first, and
                            // move the order over to the courier queue.
                            int peso = 0;
                            ing = ricetta->ingredienti;
                            while(ing != NULL){
                                qt = ing->quantita * numero;
                                peso += qt;
                                LottoMag *lottoMag = trovaLottoIngMag(magazzino, ing->idx1, ing->idx2);
                                if(lottoMag != NULL){
                                    int restante = qt;
                                    
                                    while(restante > 0){
                                        Lotto* lotto = estraMin(lottoMag);
                                        if(lotto != NULL){
                                                if(lotto->quantita <= restante){
                                                    restante -= lotto->quantita;
                                                    free(lotto);
                                                }else{
                                                    // Batch only partially used: put the
                                                    // remainder back on the heap.
                                                    lotto->quantita -= restante;
                                                    if(lotto->quantita > 0){
                                                        insMinHeap(lottoMag, lotto);
                                                        lottoMag->quantita_totale += lotto->quantita;
                                                    }
                                                    restante = 0;
                                                }
                                            
                                        }
                                        
                                    }
                                }
                                
                                ing = ing->next;
                            }
                            o->peso = peso;
                            enqueueCorriere(corriere, o);
                            corrente = corrente->next;
                            rimuoviOrdine(attesa, o);
                            i = i + 1;
                            continue;
                        }
                            
                        }
                        corrente = corrente->next;
                        i++;
                }
            }
            t++;
        }

        }else if(comando[0] == 'o'){
        if(strcmp("ordine", comando) == 0){
            char nome[LEN_NOMI];
            int numero;
            if (scanf("%s %d", nome, &numero) != 2) {
                perror("Errore durante la lettura del nome della ricetta");
                return 1;
            }
            
            Ricetta *ricetta = trovaRicetta(ricettario, nome);
            if(ricetta){
                // Recipe exists: the order is accepted regardless of stock
                // (it just might end up waiting).
                printf("accettato\n");
                Ingrediente *ing = ricetta->ingredienti;
                int possibile = 1;
                int qt;
                Ordine *o = malloc(sizeof(Ordine));
                if(!o){
                    perror("Errore allocazione memoria");
                }
                o->quantita = numero;
                o->ric = ricetta;
                o->ist = t;
                
                // Check whether we currently have enough of every
                // ingredient, sweeping expired batches along the way.
                while((ing != NULL) && possibile){
                    qt = ing->quantita * numero;
                        LottoMag *lottoMag = trovaLottoIngMag(magazzino, ing->idx1, ing->idx2);
                        if(lottoMag != NULL){
                                if(qt > lottoMag->quantita_totale){
                                            possibile = 0;
                                        }else{
                                            rimuoviScaduti(lottoMag, t);
                                            if(qt > lottoMag->quantita_totale){
                                                possibile = 0;
                                            }
                                        }
                        }else{
                            possibile = 0;
                        }
                    ing = ing->next;
                }
                if(possibile){
                    int peso = 0;
                    ing = ricetta->ingredienti;
                    while(ing != NULL){
                        qt = ing->quantita * numero;
                        peso += qt;
                        LottoMag *lottoMag = trovaLottoIngMag(magazzino, ing->idx1, ing->idx2);
                        if(lottoMag != NULL){
                            int restante = qt;
                            
                            while(restante > 0){
                                
                                Lotto* lotto = estraMin(lottoMag);
                                if(lotto != NULL){
                                        if(lotto->quantita <= restante){
                                            restante -= lotto->quantita;
                                            free(lotto);
                                        }else{ 
                                            lotto->quantita -= restante;
                                            if(lotto->quantita > 0){
                                                insMinHeap(lottoMag, lotto);
                                                lottoMag->quantita_totale += lotto->quantita;
                                            }
                                            
                                            restante = 0;
                                        }
                                }
                                
                            }
                        }
                        
                        ing = ing->next;
                    }
                    o->peso = peso;
                    enqueueCorriere(corriere, o);
                }

                if(!possibile){
                    enqueue(attesa, o);
                }
                

                }else{
                printf("rifiutato\n");
                }
                t++;
        }
        
    }
    }
    // One last courier pass after the input ends, in case the final
    // instant also lands on a pickup slot.
    if(t>1 && ((t % periodo) == 0)){
            if(t>1 && ((t % periodo) == 0)){
            int qty = 0;
            int poss = 1;
            if (corriere->size != 0){
           
                
                while(poss && (corriere->size != 0)){
                    Ordine *o = corriere->head->ordine;
                    qty += o->peso;
                    if(qty >= capienza){
                        poss = 0;
                    }else{
                        o = dequeueCorriere(corriere);
                        insMaxHeapCorriere(heap, o);
                    }
                }
              

            Ordine *o = malloc(sizeof(Ordine));
            do{
                o = estraiMaxCorriere(heap);
                if(o != NULL){
                    printf("%d %s %d\n", o->ist, o->ric->nome, o->quantita);
                }
                free(o);
            }while(heap->dim != 0);

            }else{
                printf("camioncino vuoto\n");
            }
        }
}
}
