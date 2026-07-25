#include "kz_prac.h"

void KZPracService::Init() {}

void KZPracService::Reset()
{
	this->inPrac = false;
	this->frozen = {};
	this->points.RemoveAll();
	this->currentIndex = 0;
}
