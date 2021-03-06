#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "common.hpp"
#include "annotation.hpp"
#include "read_compressed_file.hpp"
#include "filter_blacklisted_ranges.hpp"

using namespace std;

enum blacklist_item_type_t { BLACKLIST_RANGE, BLACKLIST_POSITION, BLACKLIST_GENE, BLACKLIST_ANY, BLACKLIST_SPLIT_READ_DONOR, BLACKLIST_SPLIT_READ_ACCEPTOR, BLACKLIST_SPLIT_READ_ANY, BLACKLIST_DISCORDANT_MATES, BLACKLIST_READ_THROUGH, BLACKLIST_LOW_SUPPORT, BLACKLIST_FILTER_SPLICED, BLACKLIST_NOT_BOTH_SPLICED };
struct blacklist_item_t {
	blacklist_item_type_t type;
	bool strand_defined;
	strand_t strand;
	contig_t contig;
	position_t start;
	position_t end;
	gene_t gene;
};

// convert string representation of a range into coordinates
bool parse_range(string range, const contigs_t& contigs, blacklist_item_t& blacklist_item) {
	istringstream iss;

	// extract contig from range
	string contig_name;
	replace(range.begin(), range.end(), ':', ' ');
	iss.str(range);
	iss >> contig_name;
	if (contig_name.empty()) {
		cerr << "WARNING: unknown gene or malformed range: " << range << endl;
		return false;
	}
	if (contig_name[0] == '+') {
		blacklist_item.strand_defined = true;
		blacklist_item.strand = FORWARD;
		contig_name = contig_name.substr(1);
	} else if (contig_name[0] == '-') {
		blacklist_item.strand_defined = true;
		blacklist_item.strand = REVERSE;
		contig_name = contig_name.substr(1);
	} else {
		blacklist_item.strand_defined = false;
	}
	if (contigs.find(contig_name) == contigs.end()) {
		cerr << "WARNING: unknown gene or malformed range: " << range << endl;
		return false;
	} else {
		blacklist_item.contig = contigs.at(contig_name);
	}

	// extract start (and end) of range
	if (range.find("-") != string::npos) { // range has start and end (chr:start-end)
		replace(range.begin(), range.end(), '-', ' ');
		iss.str(range);
		iss >> contig_name; // discard contig
		if ((iss >> blacklist_item.start).fail() || (iss >> blacklist_item.end).fail()) {
			cerr << "WARNING: unknown gene or malformed range: " << range << endl;
			return false;
		}
		blacklist_item.start--; // convert to zero-based coordinate
		blacklist_item.end--;

	} else { // range is a single base (chr:position)
		if ((iss >> blacklist_item.start).fail()) {
			cerr << "WARNING: unknown gene or malformed range: " << range << endl;
			return false;
		}
		blacklist_item.start--; // convert to zero-based coordinate
		blacklist_item.end = blacklist_item.start;
	}

	return true;
}

// parse string representation of a blacklist rule
bool parse_blacklist_item(const string& text, blacklist_item_t& blacklist_item, const contigs_t& contigs, const unordered_map<string,gene_t>& genes, const bool allow_keyword) {

	if (allow_keyword) {
		if (text == "any") { blacklist_item.type = BLACKLIST_ANY; return true; }
		else if (text == "split_read_donor") { blacklist_item.type = BLACKLIST_SPLIT_READ_DONOR; return true; }
		else if (text == "split_read_acceptor") { blacklist_item.type = BLACKLIST_SPLIT_READ_ACCEPTOR; return true; }
		else if (text == "split_read_any") { blacklist_item.type = BLACKLIST_SPLIT_READ_ANY; return true; }
		else if (text == "discordant_mates") { blacklist_item.type = BLACKLIST_DISCORDANT_MATES; return true; }
		else if (text == "read_through") { blacklist_item.type = BLACKLIST_READ_THROUGH; return true; }
		else if (text == "low_support") { blacklist_item.type = BLACKLIST_LOW_SUPPORT; return true; }
		else if (text == "filter_spliced") { blacklist_item.type = BLACKLIST_FILTER_SPLICED; return true; }
		else if (text == "not_both_spliced") { blacklist_item.type = BLACKLIST_NOT_BOTH_SPLICED; return true; }
	}

	auto gene = genes.find(text); // check if text is a gene name
	if (gene != genes.end()) {
		blacklist_item.type = BLACKLIST_GENE;
		blacklist_item.gene = gene->second;
		blacklist_item.contig = gene->second->contig;
		blacklist_item.start = gene->second->start;
		blacklist_item.end = gene->second->end;
	} else if (parse_range(text, contigs, blacklist_item)) { // text is a range
		if (blacklist_item.start == blacklist_item.end) {
			blacklist_item.type = BLACKLIST_POSITION;
		} else {
			blacklist_item.type = BLACKLIST_RANGE;
		}
	} else {
		return false; // failed to parse text
	}
	return true; // parsed text successfully

}

// returns the fraction of range1 that overlaps range2
float overlapping_fraction(const position_t start1, const position_t end1, const position_t start2, const position_t end2) {
	if (start1 >= start2 && end1 <= end2) {
		return 1;
	} else if (start1 >= start2 && start1 <= end2) {
		return 1.0 * (start1 - start2) / (end1 - start1 + 1);
	} else if (end1 >= start2 && end1 <= end2) {
		return 1.0 * (end2 - end1) / (end1 - start1 + 1);
	} else {
		return 0;
	}
}

// check if the breakpoint of a fusion match an entry in the blacklist
bool matches_blacklist_item(const blacklist_item_t& blacklist_item, const fusion_t& fusion, const char which_breakpoint, const float evalue_cutoff, const int max_mate_gap) {

	switch (blacklist_item.type) {
		case BLACKLIST_ANY: // remove the fusion if one breakpoint is within a region that is completely blacklisted
			return true;
		case BLACKLIST_SPLIT_READ_DONOR: // remove fusions which are only supported by donor split reads
			return (which_breakpoint == 1 && fusion.discordant_mates + fusion.split_reads1 == 0 ||
			        which_breakpoint == 2 && fusion.discordant_mates + fusion.split_reads2 == 0);
		case BLACKLIST_SPLIT_READ_ACCEPTOR: // remove fusions which are only supported by acceptor split reads
			return (which_breakpoint == 1 && fusion.discordant_mates + fusion.split_reads2 == 0 ||
			        which_breakpoint == 2 && fusion.discordant_mates + fusion.split_reads1 == 0);
		case BLACKLIST_SPLIT_READ_ANY: // remove fusions which are only supported by split reads
			return (fusion.discordant_mates == 0);
		case BLACKLIST_DISCORDANT_MATES: // remove fusions which are only supported by discordant mates
			return (fusion.split_reads1 + fusion.split_reads2 == 0);
		case BLACKLIST_READ_THROUGH: // remove read-through fusions
			return fusion.is_read_through();
		case BLACKLIST_LOW_SUPPORT: // remove recurrent speculative fusions that were recovered for one or the other reason
			return (fusion.evalue > evalue_cutoff);
		case BLACKLIST_FILTER_SPLICED: // remove recurrent speculative fusions that were recovered by the 'spliced' filter
			return (fusion.evalue > evalue_cutoff && fusion.spliced1 && fusion.spliced2);
		case BLACKLIST_NOT_BOTH_SPLICED: // remove fusions which do not have both breakpoints at splice-sites
			return (!fusion.spliced1 || !fusion.spliced2);
		case BLACKLIST_GENE: // remove blacklisted gene
			return (which_breakpoint == 1 && fusion.gene1 == blacklist_item.gene ||
			        which_breakpoint == 2 && fusion.gene2 == blacklist_item.gene);
		case BLACKLIST_POSITION: { // remove blacklisted breakpoint

			// contig must match
			contig_t contig = (which_breakpoint == 1) ? fusion.contig1 : fusion.contig2;
			if (contig != blacklist_item.contig)
				return false;

			// strand must match, if defined
			if (blacklist_item.strand_defined) {
				if (!fusion.predicted_strands_ambiguous) { // assume match, if strands could not be predicted
					strand_t strand = (which_breakpoint == 1) ? fusion.predicted_strand1 : fusion.predicted_strand2;
					if (strand != blacklist_item.strand)
						return false;
				}
			}

			// exact breakpoint must match
			position_t breakpoint = (which_breakpoint == 1) ? fusion.breakpoint1 : fusion.breakpoint2;
			if (breakpoint == blacklist_item.start)
				return true;

			// if the fusion has no split reads, then we discard it, if the discordant mates are near a blacklisted breakpoint
			// and point towards it
			if (fusion.split_reads1 + fusion.split_reads2 == 0) {
				direction_t direction = (which_breakpoint == 1) ? fusion.direction1 : fusion.direction2;
				if (direction == DOWNSTREAM && breakpoint <= blacklist_item.start && breakpoint >= blacklist_item.start - max_mate_gap ||
				    direction == UPSTREAM   && breakpoint >= blacklist_item.start && breakpoint <= blacklist_item.start + max_mate_gap)
					return true;
			}

			return false; // blacklist item does not match
		}

		case BLACKLIST_RANGE: { // remove blacklisted range

			// contig must match
			contig_t contig = (which_breakpoint == 1) ? fusion.contig1 : fusion.contig2;
			if (contig != blacklist_item.contig)
				return false;

			// strand must match, if defined
			if (blacklist_item.strand_defined) {
				if (!fusion.predicted_strands_ambiguous) { // assume match, if strands could not be predicted
					strand_t strand = (which_breakpoint == 1) ? fusion.predicted_strand1 : fusion.predicted_strand2;
					if (strand != blacklist_item.strand)
						return false;
				}
			}

			// check if the gene that the breakpoint is associated with overlaps the blacklisted range
			gene_t gene = (which_breakpoint == 1) ? fusion.gene1 : fusion.gene2;
			if (overlapping_fraction(gene->start, gene->end, blacklist_item.start, blacklist_item.end) > 0.5)
				return true;

			return false; // blacklist item does not match
		}
	}

	return false; // blacklist item does not match
}

// divide the genome into buckets of ...bps in size
void get_index_keys_from_range(const contig_t contig, const position_t start, const position_t end, vector< tuple<contig_t,position_t> >& index_keys) {
	const int bucket_size = 100000; // bp
	for (position_t position = start/bucket_size; position <= (end+bucket_size-1)/bucket_size /*integer ceil*/; ++position)
		index_keys.push_back(make_tuple(contig, position*bucket_size));
}

unsigned int filter_blacklisted_ranges(fusions_t& fusions, const string& blacklist_file_path, const contigs_t& contigs, const unordered_map<string,gene_t>& genes, const float evalue_cutoff, const int max_mate_gap) {

	// index fusions by coordinate
	unordered_map< tuple<contig_t,position_t>, set<fusion_t*> > fusions_by_coordinate;
	for (fusions_t::iterator fusion = fusions.begin(); fusion != fusions.end(); ++fusion) {

		if (fusion->second.filter != NULL && fusion->second.closest_genomic_breakpoint1 < 0)
			continue; // fusion has already been filtered and won't be recovered by the 'genomic_support' filter

		// assign fusions within a window of ...bps to the same index key
		// for fast lookup of fusions by coordinate
		vector< tuple<contig_t,position_t> > index_keys;
		get_index_keys_from_range(fusion->second.contig1, fusion->second.breakpoint1, fusion->second.breakpoint1, index_keys);
		get_index_keys_from_range(fusion->second.contig2, fusion->second.breakpoint2, fusion->second.breakpoint2, index_keys);
		get_index_keys_from_range(fusion->second.contig1, fusion->second.gene1->start, fusion->second.gene1->end, index_keys);
		get_index_keys_from_range(fusion->second.contig2, fusion->second.gene2->start, fusion->second.gene2->end, index_keys);
		for (auto index_key = index_keys.begin(); index_key != index_keys.end(); ++index_key)
			fusions_by_coordinate[*index_key].insert(&(fusion->second));
	}

	// load blacklist from file
	stringstream blacklist_file;
	autodecompress_file(blacklist_file_path, blacklist_file);
	string line;
	while (getline(blacklist_file, line)) {

		// skip comment lines
		if (line.empty() || line[0] == '#')
			continue;

		// parse line
		istringstream iss(line);
		string range1, range2;
		iss >> range1 >> range2;
		blacklist_item_t item1, item2;
		if (!parse_blacklist_item(range1, item1, contigs, genes, false) ||
		    !parse_blacklist_item(range2, item2, contigs, genes, true))
			continue;

		// find all fusions with breakpoints in the vicinity of the blacklist items
		vector< tuple<contig_t,position_t> > index_keys;
		if (item1.type == BLACKLIST_POSITION || item1.type == BLACKLIST_RANGE || item1.type == BLACKLIST_GENE)
			get_index_keys_from_range(item1.contig, item1.start-max_mate_gap, item1.end+max_mate_gap, index_keys);
		if (item2.type == BLACKLIST_POSITION || item2.type == BLACKLIST_RANGE || item2.type == BLACKLIST_GENE)
			get_index_keys_from_range(item2.contig, item2.start-max_mate_gap, item2.end+max_mate_gap, index_keys);
		for (auto index_key = index_keys.begin(); index_key != index_keys.end(); ++index_key) {
			auto fusions_near_coordinate = fusions_by_coordinate.find(*index_key);
			if (fusions_near_coordinate != fusions_by_coordinate.end()) {
				for (auto fusion = fusions_near_coordinate->second.begin(); fusion != fusions_near_coordinate->second.end();) {
					if (matches_blacklist_item(item1, **fusion, 1, evalue_cutoff, max_mate_gap) &&
					    matches_blacklist_item(item2, **fusion, 2, evalue_cutoff, max_mate_gap) ||
					    matches_blacklist_item(item1, **fusion, 2, evalue_cutoff, max_mate_gap) &&
					    matches_blacklist_item(item2, **fusion, 1, evalue_cutoff, max_mate_gap)) {
						(**fusion).filter = FILTERS.at("blacklist");
						fusions_near_coordinate->second.erase(fusion++); // remove fusion from index, so we don't check it again
					} else {
						++fusion;
					}
				}
			}
		}
	}

	// count remaining fusions
	unsigned int remaining = 0;
	for (fusions_t::iterator fusion = fusions.begin(); fusion != fusions.end(); ++fusion)
		if (fusion->second.filter == NULL)
			remaining++;
	return remaining;
}

