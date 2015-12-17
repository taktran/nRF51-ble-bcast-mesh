#include <stdio.h>
#include <string.h>
#include "bootloader_info.h"
#include "bootloader_mesh.h"
#include "dfu_types_mesh.h"
#include "nrf_flash.h"
#include "app_error.h"
#include "nrf51.h"

#define WORD_ALIGN(data) data = (((uint32_t) data + 4) & 0xFFFFFFFC)

#define HEADER_LEN       (4)

#ifdef DEBUG
#undefine DEBUG
#define DEBUG 0
#endif

#define PIN_INVALIDATE      (0)
#define PIN_RESET           (1)
#define PIN_ENTRY_GET       (2)
#define PIN_SET_LEN         (3)
#define PIN_ENTRY_PUT       (4)
#define PIN_INIT            (5)

#if defined(NRF51) && DEBUG
#define PIN_SET(x) NRF_GPIO->OUTSET = (1 << (x))
#define PIN_CLEAR(x) NRF_GPIO->OUTCLR = (1 << (x))
#define PIN_TICK(x) do{ PIN_SET(x); _NOP(); _NOP(); _NOP(); _NOP(); PIN_CLEAR(x); while (0)
#else
#define PIN_SET(x) 
#define PIN_CLEAR(x)
#define PIN_TICK(x) 
#endif

typedef struct
{
    uint16_t len;
    uint16_t type;
} bootloader_info_header_t;

static bootloader_info_t* mp_bl_info_page;
static bootloader_info_t* mp_bl_info_bank_page;
/******************************************************************************
* Static functions
******************************************************************************/
static void invalidate_entry(bootloader_info_header_t* p_header)
{
    PIN_SET(PIN_INVALIDATE);
    /* TODO: optimization: check if the write only adds 0-bits to the current value,
       in which case we can just overwrite the current. */
    if ((uint32_t) p_header & 0x03)
    {
        APP_ERROR_CHECK(NRF_ERROR_INVALID_ADDR);
    }

    bootloader_info_header_t old_header_ram;
    memcpy(&old_header_ram, p_header, HEADER_LEN);
    old_header_ram.type = BL_INFO_TYPE_INVALID;

    uint32_t len = HEADER_LEN;
    nrf_flash_store((uint32_t*) p_header, (uint8_t*) &old_header_ram, len, 0);
    PIN_CLEAR(PIN_INVALIDATE);
}

static inline bootloader_info_header_t* bootloader_info_iterate(bootloader_info_header_t* p_info_header)
{
    return (bootloader_info_header_t*) (((uint32_t) p_info_header) + ((uint32_t) p_info_header->len) * 4);
}

static bootloader_info_header_t* bootloader_info_header_get(bl_info_entry_t* p_entry)
{
    if (p_entry == NULL || (uint32_t) p_entry == FLASH_SIZE)
    {
        return NULL;
    }
    return (bootloader_info_header_t*) ((uint32_t) p_entry - mp_bl_info_page->metadata.entry_header_length);
}

static inline bootloader_info_header_t* bootloader_info_first_unused_get(bootloader_info_t* p_bl_info_page)
{
    bl_info_entry_t* p_entry = bootloader_info_entry_get(
            (uint32_t*) p_bl_info_page,
            BL_INFO_TYPE_LAST);

    return (p_entry ? bootloader_info_header_get(p_entry) : NULL);
}

static bootloader_info_header_t* reset_with_replace(
        bl_info_type_t replace_type,
        bl_info_entry_t* p_entry,
        uint32_t entry_length)
{
    PIN_SET(PIN_RESET);
    /* create new page from valid entries, assumes intact info page */
    uint8_t new_page[PAGE_SIZE];
    bootloader_info_header_t* p_replace_position = NULL;
    memset(new_page, 0xFF, PAGE_SIZE);
    memcpy(new_page, mp_bl_info_page, mp_bl_info_page->metadata.metadata_len);
    uint8_t* p_new_page_next_space = &new_page[mp_bl_info_page->metadata.metadata_len];

    bootloader_info_header_t* p_info =
        ((bootloader_info_header_t*)
         ((uint8_t*) mp_bl_info_page + mp_bl_info_page->metadata.metadata_len));

    while ((uint32_t) p_info < (uint32_t) ((uint32_t) mp_bl_info_page + PAGE_SIZE))
    {
        bl_info_type_t type = (bl_info_type_t) p_info->type;

        if (type == BL_INFO_TYPE_LAST)
        {
            break;
        }

        if (type != BL_INFO_TYPE_INVALID)
        {
            if (type == replace_type)
            {
                /* replace */
                p_replace_position = (bootloader_info_header_t*) p_new_page_next_space;
                uint32_t len = HEADER_LEN + entry_length;
                if (len & 0x03)
                {
                    WORD_ALIGN(len);
                }

                if (p_new_page_next_space + len > new_page + PAGE_SIZE)
                {
                    APP_ERROR_CHECK(NRF_ERROR_INTERNAL);
                }
                p_replace_position->len = len / 4;
                p_replace_position->type = type;
                memset(p_new_page_next_space + len - 3, 0xFF, 3); /* sanitize padding */
                memcpy(p_new_page_next_space + HEADER_LEN, p_entry, entry_length);
                p_new_page_next_space += len;
            }
            else
            {
                /* restore */
                uint32_t len = p_info->len * 4;
                if (p_new_page_next_space + len > new_page + PAGE_SIZE)
                {
                    APP_ERROR_CHECK(NRF_ERROR_INTERNAL);
                }
                memcpy(p_new_page_next_space, p_info, len);
                p_new_page_next_space += len;
            }
        }
        p_info = bootloader_info_iterate(p_info);
    }

    if (p_new_page_next_space < &new_page[PAGE_SIZE])
    {
        ((bootloader_info_header_t*) p_new_page_next_space)->type = BL_INFO_TYPE_LAST;
    }

    /* bank page */
    nrf_flash_erase((uint32_t*) mp_bl_info_bank_page, PAGE_SIZE);
    nrf_flash_store((uint32_t*) mp_bl_info_bank_page, new_page, PAGE_SIZE, 0);

    /* reflash original */
    nrf_flash_erase((uint32_t*) mp_bl_info_page, PAGE_SIZE);
    nrf_flash_store((uint32_t*) mp_bl_info_page, new_page, PAGE_SIZE, 0);

    PIN_CLEAR(PIN_RESET);
    return p_replace_position;
}
/******************************************************************************
* Interface functions
******************************************************************************/
uint32_t bootloader_info_init(uint32_t* p_bl_info_page, uint32_t* p_bl_info_bank_page)
{
    PIN_SET(PIN_INIT);
    if ((uint32_t) p_bl_info_page & (PAGE_SIZE - 1) ||
        (uint32_t) p_bl_info_bank_page & (PAGE_SIZE - 1))
    {
        return NRF_ERROR_INVALID_ADDR; /* must be page aligned */
    }

    mp_bl_info_page      = (bootloader_info_t*) p_bl_info_page;
    mp_bl_info_bank_page = (bootloader_info_t*) p_bl_info_bank_page;

    /* make sure we have an end-of-entries entry */
    if (bootloader_info_first_unused_get(mp_bl_info_page) == NULL)
    {
        /* need to restore the bank */
        if (bootloader_info_first_unused_get(mp_bl_info_bank_page) == NULL)
        {
            /* bank is invalid too, no way to recover */
            bootloader_abort(BL_END_ERROR_INVALID_PERSISTANT_STORAGE);
        }

        nrf_flash_erase((uint32_t*) mp_bl_info_page, PAGE_SIZE);
        nrf_flash_store((uint32_t*) mp_bl_info_page, (uint8_t*) mp_bl_info_bank_page, PAGE_SIZE, 0);
    }

    PIN_CLEAR(PIN_INIT);
    return NRF_SUCCESS;
}

bootloader_info_t* bootloader_info_get(void)
{
    return (bootloader_info_t*) (mp_bl_info_page);
}

bl_info_entry_t* bootloader_info_entry_get(uint32_t* p_bl_info_page, bl_info_type_t type)
{
    PIN_SET(PIN_ENTRY_GET);
    bootloader_info_header_t* p_header =
        (bootloader_info_header_t*)
        ((uint32_t) p_bl_info_page + ((bootloader_info_t*) p_bl_info_page)->metadata.metadata_len);
    uint32_t iterations = 0;
    bl_info_type_t iter_type = (bl_info_type_t) p_header->type;
    while (iter_type != type)
    {
        p_header = bootloader_info_iterate(p_header);
        if ((uint32_t) p_header > ((uint32_t) p_bl_info_page) + PAGE_SIZE ||
            (
             (iter_type == BL_INFO_TYPE_LAST) &&
             (iter_type != type)
            ) ||
            ++iterations > PAGE_SIZE / 2)
        {
            PIN_CLEAR(PIN_ENTRY_GET);
            return NULL; /* out of bounds */
        }
        iter_type = (bl_info_type_t) p_header->type;
    }
    PIN_CLEAR(PIN_ENTRY_GET);
    return (bl_info_entry_t*) ((uint32_t) p_header + mp_bl_info_page->metadata.entry_header_length);
}

bl_info_entry_t* bootloader_info_entry_put(bl_info_type_t type,
                                           bl_info_entry_t* p_entry,
                                           uint32_t length)
{
    if (PAGE_ALIGN((uint32_t) p_entry) == (uint32_t) mp_bl_info_page) /* Can't come from our info page */
    {
        return NULL;
    }
    PIN_SET(PIN_ENTRY_PUT);

    /* previous entry, to be invalidated after we've stored the current entry. */
    bootloader_info_header_t* p_old_header =
        bootloader_info_header_get(bootloader_info_entry_get((uint32_t*) mp_bl_info_page, type));

    /* find first unused space */
    bootloader_info_header_t* p_new_header =
        bootloader_info_first_unused_get(mp_bl_info_page);

    /* we have to be able to fit the end-of-entries entry at the last word, therefore check entry
       length + 4 */
    if (p_new_header == NULL ||
        ((uint32_t) p_new_header + HEADER_LEN + length + 4) >= (uint32_t) ((uint32_t) mp_bl_info_page + PAGE_SIZE))
    {
        /* entry page has overflowed. A reset is needed. */
        p_new_header = reset_with_replace(type, p_entry, length);

        p_old_header = NULL;
    }
    else
    {
        if ((uint32_t) p_new_header & 0x03)
        {
            APP_ERROR_CHECK(NRF_ERROR_INVALID_ADDR);
        }

        /* store new entry in the available space */
        uint8_t buffer[PAGE_SIZE];
        uint32_t entry_length = HEADER_LEN + length;
        if (entry_length & 0x03)
        {
            WORD_ALIGN(entry_length);
        }

        memset(&buffer[entry_length - 4], 0xFF, 8); /* pad the end-of-entries entry */
        ((bootloader_info_header_t*) &buffer[0])->type = type;
        ((bootloader_info_header_t*) &buffer[0])->len = (length + HEADER_LEN) / 4;
        memcpy(&buffer[HEADER_LEN], p_entry, length);

        /* add end-of-entries-entry */
        ((bootloader_info_header_t*) &buffer[entry_length])->type = BL_INFO_TYPE_LAST;

        nrf_flash_store((uint32_t*) p_new_header, buffer, entry_length + 4, 0);

        /* invalidate old entry of this type */
        if (p_old_header != NULL)
        {
            invalidate_entry(p_old_header);
        }
    }
    PIN_CLEAR(PIN_ENTRY_PUT);
    return (bl_info_entry_t*) ((uint32_t) p_new_header + mp_bl_info_page->metadata.entry_header_length);
}

void bootloader_info_reset(void)
{
    reset_with_replace(BL_INFO_TYPE_INVALID, NULL, 0);
}
