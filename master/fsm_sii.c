/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2006  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it
 *  and/or modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be
 *  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  The right to use EtherCAT Technology is granted and comes free of
 *  charge under condition of compatibility of product made by
 *  Licensee. People intending to distribute/sell products based on the
 *  code, have to sign an agreement to guarantee that products using
 *  software based on IgH EtherCAT master stay compatible with the actual
 *  EtherCAT specification (which are released themselves as an open
 *  standard) as the (only) precondition to have the right to use EtherCAT
 *  Technology, IP and trade marks.
 *
 *****************************************************************************/

/**
   \file
   EtherCAT slave information interface FSM.
*/

/*****************************************************************************/

#include "globals.h"
#include "mailbox.h"
#include "master.h"
#include "fsm_sii.h"

/**
 * Read/Write timeout. [ms]
 */
#define EEPROM_TIMEOUT 10

/**
 * Time before evaluating answer at writing. [ms]
 */
#define EEPROM_INHIBIT  5

//#define SII_DEBUG

/*****************************************************************************/

void ec_fsm_sii_state_start_reading(ec_fsm_sii_t *);
void ec_fsm_sii_state_read_check(ec_fsm_sii_t *);
void ec_fsm_sii_state_read_fetch(ec_fsm_sii_t *);
void ec_fsm_sii_state_start_writing(ec_fsm_sii_t *);
void ec_fsm_sii_state_write_check(ec_fsm_sii_t *);
void ec_fsm_sii_state_write_check2(ec_fsm_sii_t *);
void ec_fsm_sii_state_end(ec_fsm_sii_t *);
void ec_fsm_sii_state_error(ec_fsm_sii_t *);

/*****************************************************************************/

/**
   Constructor.
*/

void ec_fsm_sii_init(ec_fsm_sii_t *fsm, /**< finite state machine */
                     ec_datagram_t *datagram /**< datagram structure to use */
                     )
{
    fsm->state = NULL;
    fsm->datagram = datagram;
}

/*****************************************************************************/

/**
   Destructor.
*/

void ec_fsm_sii_clear(ec_fsm_sii_t *fsm /**< finite state machine */)
{
}

/*****************************************************************************/

/**
   Initializes the SII read state machine.
*/

void ec_fsm_sii_read(ec_fsm_sii_t *fsm, /**< finite state machine */
                     ec_slave_t *slave, /**< slave to read from */
                     uint16_t word_offset, /**< offset to read from */
                     ec_fsm_sii_addressing_t mode /**< addressing scheme */
                     )
{
    fsm->state = ec_fsm_sii_state_start_reading;
    fsm->slave = slave;
    fsm->word_offset = word_offset;
    fsm->mode = mode;
}

/*****************************************************************************/

/**
   Initializes the SII write state machine.
*/

void ec_fsm_sii_write(ec_fsm_sii_t *fsm, /**< finite state machine */
                      ec_slave_t *slave, /**< slave to read from */
                      uint16_t word_offset, /**< offset to read from */
                      const uint8_t *value, /**< pointer to 2 bytes of data */
                      ec_fsm_sii_addressing_t mode /**< addressing scheme */
                      )
{
    fsm->state = ec_fsm_sii_state_start_writing;
    fsm->slave = slave;
    fsm->word_offset = word_offset;
    fsm->mode = mode;
    memcpy(fsm->value, value, 2);
}

/*****************************************************************************/

/**
   Executes the SII state machine.
   \return false, if the state machine has terminated
*/

int ec_fsm_sii_exec(ec_fsm_sii_t *fsm /**< finite state machine */)
{
    fsm->state(fsm);

    return fsm->state != ec_fsm_sii_state_end
		&& fsm->state != ec_fsm_sii_state_error;
}

/*****************************************************************************/

/**
   Returns, if the master startup state machine terminated with success.
   \return non-zero if successful.
*/

int ec_fsm_sii_success(ec_fsm_sii_t *fsm /**< Finite state machine */)
{
    return fsm->state == ec_fsm_sii_state_end;
}

/******************************************************************************
 * state functions
 *****************************************************************************/

/**
   SII state: START READING.
   Starts reading the slave information interface.
*/

void ec_fsm_sii_state_start_reading(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    // initiate read operation
    switch (fsm->mode) {
        case EC_FSM_SII_POSITION:
            ec_datagram_apwr(datagram, fsm->slave->ring_position, 0x502, 4);
            break;
        case EC_FSM_SII_NODE:
            ec_datagram_npwr(datagram, fsm->slave->station_address, 0x502, 4);
            break;
    }

    EC_WRITE_U8 (datagram->data,     0x80); // two address octets
    EC_WRITE_U8 (datagram->data + 1, 0x01); // request read operation
    EC_WRITE_U16(datagram->data + 2, fsm->word_offset);

#ifdef SII_DEBUG
	EC_DBG("reading SII data:\n");
	ec_print_data(datagram->data, 4);
#endif

    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_sii_state_read_check;
}

/*****************************************************************************/

/**
   SII state: READ CHECK.
   Checks, if the SII-read-datagram has been sent and issues a fetch datagram.
*/

void ec_fsm_sii_state_read_check(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Failed to receive SII read datagram from slave %i"
                " (datagram state %i).\n",
               fsm->slave->ring_position, datagram->state);
        return;
    }

    if (datagram->working_counter != 1) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Reception of SII read datagram failed on slave %i: ",
                fsm->slave->ring_position);
        ec_datagram_print_wc_error(datagram);
        return;
    }

    fsm->cycles_start = datagram->cycles_sent;
    fsm->check_once_more = 1;

    // issue check/fetch datagram
    switch (fsm->mode) {
        case EC_FSM_SII_POSITION:
            ec_datagram_aprd(datagram, fsm->slave->ring_position, 0x502, 10);
            break;
        case EC_FSM_SII_NODE:
            ec_datagram_nprd(datagram, fsm->slave->station_address, 0x502, 10);
            break;
    }

    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_sii_state_read_fetch;
}

/*****************************************************************************/

/**
   SII state: READ FETCH.
   Fetches the result of an SII-read datagram.
*/

void ec_fsm_sii_state_read_fetch(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Failed to receive SII check/fetch datagram from slave %i"
                " (datagram state %i).\n",
               fsm->slave->ring_position, datagram->state);
        return;
    }

    if (datagram->working_counter != 1) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Reception of SII check/fetch datagram failed on slave %i: ",
                fsm->slave->ring_position);
        ec_datagram_print_wc_error(datagram);
        return;
    }

#ifdef SII_DEBUG
	EC_DBG("checking SII read state:\n");
	ec_print_data(datagram->data, 10);
#endif

    if (EC_READ_U8(datagram->data + 1) & 0x20) {
        EC_ERR("SII: Error on last SII command!\n");
        fsm->state = ec_fsm_sii_state_error;
        return;
    }

    // check "busy bit"
    if (EC_READ_U8(datagram->data + 1) & 0x81) { // busy bit or
												 // read operation busy
        // still busy... timeout?
        if (datagram->cycles_received
            - fsm->cycles_start >= (cycles_t) EEPROM_TIMEOUT * cpu_khz) {
            if (fsm->check_once_more) {
				fsm->check_once_more = 0;
			} else {
                EC_ERR("SII: Read timeout.\n");
                fsm->state = ec_fsm_sii_state_error;
                return;
            }
        }

        // issue check/fetch datagram again
        fsm->retries = EC_FSM_RETRIES;
        return;
    }

    // SII value received.
    memcpy(fsm->value, datagram->data + 6, 4);
    fsm->state = ec_fsm_sii_state_end;
}

/*****************************************************************************/

/**
   SII state: START WRITING.
   Starts writing a word through the slave information interface.
*/

void ec_fsm_sii_state_start_writing(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    // initiate write operation
    ec_datagram_npwr(datagram, fsm->slave->station_address, 0x502, 8);
    EC_WRITE_U8 (datagram->data,     0x81); // two address octets
											// + enable write access
    EC_WRITE_U8 (datagram->data + 1, 0x02); // request write operation
    EC_WRITE_U16(datagram->data + 2, fsm->word_offset);
	memset(datagram->data + 4, 0x00, 2);
    memcpy(datagram->data + 6, fsm->value, 2);

#ifdef SII_DEBUG
	EC_DBG("writing SII data:\n");
	ec_print_data(datagram->data, 8);
#endif

    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_sii_state_write_check;
}

/*****************************************************************************/

/**
   SII state: WRITE CHECK.
*/

void ec_fsm_sii_state_write_check(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Failed to receive SII write datagram for slave %i"
                " (datagram state %i).\n",
               fsm->slave->ring_position, datagram->state);
        return;
    }

    if (datagram->working_counter != 1) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Reception of SII write datagram failed on slave %i: ",
                fsm->slave->ring_position);
        ec_datagram_print_wc_error(datagram);
        return;
    }

    fsm->cycles_start = datagram->cycles_sent;
    fsm->check_once_more = 1;

    // issue check datagram
    ec_datagram_nprd(datagram, fsm->slave->station_address, 0x502, 2);
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_sii_state_write_check2;
}

/*****************************************************************************/

/**
   SII state: WRITE CHECK 2.
*/

void ec_fsm_sii_state_write_check2(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Failed to receive SII write check datagram from slave %i"
                " (datagram state %i).\n",
               fsm->slave->ring_position, datagram->state);
        return;
    }

    if (datagram->working_counter != 1) {
        fsm->state = ec_fsm_sii_state_error;
        EC_ERR("Reception of SII write check datagram failed on slave %i: ",
                fsm->slave->ring_position);
        ec_datagram_print_wc_error(datagram);
        return;
    }

#ifdef SII_DEBUG
	EC_DBG("checking SII write state:\n");
	ec_print_data(datagram->data, 2);
#endif

    if (EC_READ_U8(datagram->data + 1) & 0x20) {
        EC_ERR("SII: Error on last SII command!\n");
        fsm->state = ec_fsm_sii_state_error;
        return;
    }

	/* FIXME: some slaves never answer with the busy flag set...
	 * wait a few ms for the write operation to complete. */
	if (datagram->cycles_received - fsm->cycles_start
			< (cycles_t) EEPROM_INHIBIT * cpu_khz) {
#ifdef SII_DEBUG
		EC_DBG("too early.\n");
#endif
        // issue check datagram again
        fsm->retries = EC_FSM_RETRIES;
        return;
	}

    if (EC_READ_U8(datagram->data + 1) & 0x82) { // busy bit or
												 // write operation busy bit
        // still busy... timeout?
        if (datagram->cycles_received
            - fsm->cycles_start >= (cycles_t) EEPROM_TIMEOUT * cpu_khz) {
            if (fsm->check_once_more) {
				fsm->check_once_more = 0;
			} else {
                EC_ERR("SII: Write timeout.\n");
                fsm->state = ec_fsm_sii_state_error;
                return;
            }
        }

        // issue check datagram again
        fsm->retries = EC_FSM_RETRIES;
        return;
    }

    if (EC_READ_U8(datagram->data + 1) & 0x40) {
        EC_ERR("SII: Write operation failed!\n");
        fsm->state = ec_fsm_sii_state_error;
        return;
    }

    // success
    fsm->state = ec_fsm_sii_state_end;
}

/*****************************************************************************/

/**
   State: ERROR.
*/

void ec_fsm_sii_state_error(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
}

/*****************************************************************************/

/**
   State: END.
*/

void ec_fsm_sii_state_end(
		ec_fsm_sii_t *fsm /**< finite state machine */
		)
{
}

/*****************************************************************************/
