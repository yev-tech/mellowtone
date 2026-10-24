#ifndef R3PRO_STOCK_INI_H
#define R3PRO_STOCK_INI_H

#include <stdbool.h>

/* The stock player's gain in /data/user.ini, shared so
 * both players use the same volume table. Stock has two levels: High (HDB)
 * and Low, which is the MDB table Mellowtone calls Medium. Mellowtone levels are
 * 0 = HDB, 1 = MDB, 2 = LDB. */

/* Mellowtone level for the stored stock gain; a Mellowtone LDB (`current` == 2) is
 * kept when stock says Low, since stock cannot express it. False when the
 * file is missing or not the expected layout. */
bool r3pro_stock_gain_read(int current, int * out_level);
/* Writes the stock gain for a Mellowtone level (only the 4-byte field, via a temp
 * file and rename). Does nothing unless the file has the expected layout. */
bool r3pro_stock_gain_write(int level);

#endif
