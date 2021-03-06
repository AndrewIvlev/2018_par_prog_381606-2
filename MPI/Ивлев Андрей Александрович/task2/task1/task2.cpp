#include "pch.h"

/*
*	При распараллеливании алгоритма с помощью MPI сильно меняется структура кода,
*	т.к. данные (в нашем случае система линейных уравнений) будет введена только на одном узле.
*	Для передачи другим вычислительным узлам необходимо выполнить отправку сообщения.
*	Узлы получают свою часть данных, обрабатывают ее и результаты отправляют некоторому узлу, собирающему результаты.
*	В алгоритме Гаусса-Жордана удобно распараллеливать обработку элементарных преобразований над матрицей
*	(домножение строки на константу и ее сложение с другой строкой матрицы).
*/

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <mpi.h>
#include <math.h>
#include <string.h>

#define MAX_NUMBER 1000
#define ROOT 0

struct PivotChoice {
	double max;
	int procRank;
};

void processInitialization(double **a, double **b, double **resMat, double **resVec, double **procRows, double **procVec, const int n, int *rowsCount);
void dataDistribution(double *a, double *b, double *procRows, double *procVec, const int n);
void parallelGJElimination(double *procRows, double *procVec, const int n, const int rowsCount);
void parallelDirectFlow(double *procRows, double *procVec, const int n, const int rowsCount);
void parallelReverseFlow(double *procRows, double *procVec, const int n, const int rowsCount);
void parallelDirectEliminateColumns(double *procRows, double *procVec, double *pivotRow, const int n, const int rowsCount, const int i);
void parallelReverseEliminateColumns(double *procRows, double *procVec, const double pivotElem, const int n, const int rowsCount, const int i);
void collectData(double *resMat, double *resVec, double *procRows, double *procVec, const int n);
void setArraysValues(int **count, int **indices, const int n);

void divideVec(double *vec, const int n, const double div);
void cpyVec(double *source, double *dest, const int n);
void subVecs(double *minuend, double *sub, const int n, const double mult);
double *createVec(const int n, const int max);
void printMatrix(double *a, double *b, const int rowsCount, const int colsCount);
void swapRows(double *r1, double *r2, const int n);
void swap(double *v1, double *v2);

int procRank, procCount;
int *procPivotIter;

int main(int argc, char *argv[]) {
	double *a = NULL;
	double *b = NULL;
	double *resMat = NULL;
	double *resVec = NULL;
	double *procRows = NULL;
	double *procVec = NULL;
	double time, linstarttime, linendtime;
	int n = atoi(argv[1]);
	int rowsCount;

	MPI_Init(&argc, &argv);
	time = MPI_Wtime();

	MPI_Comm_rank(MPI_COMM_WORLD, &procRank);
	MPI_Comm_size(MPI_COMM_WORLD, &procCount);

	processInitialization(&a, &b, &resMat, &resVec, &procRows, &procVec, n, &rowsCount);

	if (procRank == ROOT && n < 8) {
		printf("Source matrix:\n");
		printMatrix(a, b, n, n);
	}

	dataDistribution(a, b, procRows, procVec, n);

	parallelGJElimination(procRows, procVec, n, rowsCount);

	collectData(resMat, resVec, procRows, procVec, n);

	if (procRank == ROOT) {
		time = MPI_Wtime() - time;
		printf("Work time of parallel algoritm = %f\n", time);
	}
	MPI_Finalize();

	///*Проверка
	if (procRank == ROOT) {
		double *testRes = (double *)malloc(n * sizeof(double));
		int maxPos;
		int i, j, k = 0;

		for (j = 0; j < n; ++j)
			for (i = 0; i < n; ++i)
				if (resMat[i * n + j] == 1.0) {
					swapRows(&resMat[i * n], &resMat[k * n], n);
					swap(&resVec[i], &resVec[k]);
					++k;
				}

		if (n < 8) {
			printf("Output matrix:\n");
			printMatrix(resMat, resVec, n, n);
		}

		for (i = 0; i < n; ++i)
			testRes[i] = 0.0;

		for (i = 0; i < n; ++i)
			for (j = 0; j < n; ++j)
				testRes[i] += a[i * n + j] * resVec[j];

		maxPos = 0;
		for (i = 1; i < n; ++i)
			if (fabs(testRes[maxPos] - b[maxPos]) < fabs(testRes[i] - b[i]))
				maxPos = i;

		printf("Max delta = %2.14f", fabs(testRes[maxPos] - b[maxPos]));
	}
	//*/

	return 0;
}

void processInitialization(double **a, double **b, double **resMat, double **resVec, double **procRows, double **procVec, const int n, int *rowsCount) {
	int rowsRest, i;

	rowsRest = n;
	for (i = 0; i < procRank; ++i)
		rowsRest -= rowsRest / (procCount - i);
	*rowsCount = rowsRest / (procCount - procRank);

	*procRows = (double *)malloc(*rowsCount * n * sizeof(double));
	*procVec = (double *)malloc(*rowsCount * sizeof(double));

	//printf("%d process: %d\n", procRank, *rowsCount);

	if (procRank == ROOT) {
		*a = createVec(n * n, MAX_NUMBER);
		*b = createVec(n, MAX_NUMBER);
		*resMat = (double *)malloc(n * n * sizeof(double));
		*resVec = (double *)malloc(n * sizeof(double));

		/*Вырожденный случай: (n - 2)-ая строка - нулевая
		for (i = 0; i < n; ++i)
			(*a)[(n-2) * n + i] = 0;
		(*b)[n - 2] = 0;
		*/
	}

	procPivotIter = (int *)malloc(*rowsCount * sizeof(int));
	for (i = 0; i < *rowsCount; ++i)
		procPivotIter[i] = -1;
}

void dataDistribution(double *a, double *b, double *procRows, double *procVec, const int n) {
	int *sendCount, *sendIndices, i;

	setArraysValues(&sendCount, &sendIndices, n);

	/*//Просмотреть сколько и откуда попадет процессам
	if (procRank == ROOT) {
		printf("sendCount: ");
		for (i = 0; i < procCount; ++i)
			printf("%d ", sendCount[i]);
		printf("\n");
		printf("sendIndices: ");
		for (i = 0; i < procCount; ++i)
			printf("%d ", sendIndices[i]);
		printf("\n");
	}
	*/

	MPI_Scatterv(a, sendCount, sendIndices, MPI_DOUBLE, procRows, sendCount[procRank], MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

	for (i = 0; i < procCount; ++i) {
		sendCount[i] /= n;
		sendIndices[i] /= n;
	}

	MPI_Scatterv(b, sendCount, sendIndices, MPI_DOUBLE, procVec, sendCount[procRank], MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

	/*
	printf("Process %d:\n", procRank);
	printMatrix(procRows, procVec, sendCount[procRank], n);
	*/

	free(sendCount);
	free(sendIndices);
}

void parallelGJElimination(double *procRows, double *procVec, const int n, const int rowsCount) {

	parallelDirectFlow(procRows, procVec, n, rowsCount);
	MPI_Barrier(MPI_COMM_WORLD);
	parallelReverseFlow(procRows, procVec, n, rowsCount);
}

void parallelDirectFlow(double *procRows, double *procVec, const int n, const int rowsCount) {
	double max;
	int pivotPos;
	struct PivotChoice procPivot, pivot;
	double *pivotRow = (double *)malloc((n + 1) * sizeof(double));
	int i, j;

	for (i = 0; i < n; ++i) {
		max = 0.0;
		for (j = 0; j < rowsCount; ++j)
			if (max < fabs(procRows[j * n + i]) && procPivotIter[j] == -1) {
				max = fabs(procRows[j * n + i]);
				pivotPos = j;
			}

		procPivot.max = max;
		procPivot.procRank = procRank;

		MPI_Allreduce(&procPivot, &pivot, 1, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);

		if (pivot.max != 0.0) {
			if (procRank == pivot.procRank) {
				double div = procRows[pivotPos * n + i];

				procPivotIter[pivotPos] = i;

				divideVec(&procRows[pivotPos * n], n, div);
				procVec[pivotPos] /= div;

				cpyVec(&procRows[pivotPos * n], pivotRow, n);
				pivotRow[n] = procVec[pivotPos];
			}

			MPI_Bcast(pivotRow, n + 1, MPI_DOUBLE, pivot.procRank, MPI_COMM_WORLD);

			parallelDirectEliminateColumns(procRows, procVec, pivotRow, n, rowsCount, i);
		}
	}

	free(pivotRow);
}

void parallelReverseFlow(double *procRows, double *procVec, const int n, const int rowsCount) {
	int pivotPos;
	struct PivotChoice procPivot, pivot;
	double pivotElem;
	int i, j;

	for (i = n - 1; i >= 0; --i) {
		pivotPos = 0;
		for (j = 1; j < rowsCount; ++j)
			if (procPivotIter[pivotPos] < procPivotIter[j])
				pivotPos = j;

		procPivot.max = procPivotIter[pivotPos];
		procPivot.procRank = procRank;

		MPI_Allreduce(&procPivot, &pivot, 1, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);

		if (procRank == pivot.procRank) {
			procPivotIter[pivotPos] = -1;


			pivotElem = procVec[pivotPos];
		}

		MPI_Bcast(&pivotElem, 1, MPI_DOUBLE, pivot.procRank, MPI_COMM_WORLD);

		parallelReverseEliminateColumns(procRows, procVec, pivotElem, n, rowsCount, pivot.max);
	}
}

void parallelDirectEliminateColumns(double *procRows, double *procVec, double *pivotRow, const int n, const int rowsCount, const int i) {
	int j;

	for (j = 0; j < rowsCount; ++j)
		if (procPivotIter[j] == -1) {
			double mult = procRows[j * n + i];

			subVecs(&procRows[j * n], pivotRow, n, mult);
			procVec[j] -= (mult * pivotRow[n]);
		}
}

void parallelReverseEliminateColumns(double *procRows, double *procVec, const double pivotElem, const int n, const int rowsCount, const int i) {
	int j;

	for (j = 0; j < rowsCount; ++j)
		if (procPivotIter[j] != -1) {
			double mult = procRows[j * n + i];

			procRows[j * n + i] = 0.0;
			procVec[j] -= (mult * pivotElem);
		}
}

void divideVec(double *vec, const int n, const double div) {
	int i;

	for (i = 0; i < n; ++i)
		vec[i] /= div;
}

void subVecs(double *minuend, double *sub, const int n, const double mult) {
	int i;

	for (i = 0; i < n; ++i)
		minuend[i] -= (mult * sub[i]);
}

void cpyVec(double *source, double *dest, const int n) {
	int i;

	for (i = 0; i < n; ++i)
		dest[i] = source[i];
}

void printMatrix(double *a, double *b, const int rowsCount, const int colsCount) {
	int i, j;

	for (i = 0; i < rowsCount; ++i) {
		for (j = 0; j < colsCount; ++j)
			printf("%3.2f ", a[i * colsCount + j]);

		printf("\t%3.2f\n", b[i]);
	}
}

double *createVec(const int n, const int max) {
	double *vec;
	int i;

	vec = (double *)malloc(n * sizeof(double));

	srand(time(0));

	for (i = 0; i < n; ++i)
		vec[i] = rand() % max;

	return vec;
}

void collectData(double *resMat, double *resVec, double *procRows, double *procVec, const int n) {
	int *rcvCount, *rcvIndices, i;

	setArraysValues(&rcvCount, &rcvIndices, n);

	MPI_Gatherv(procRows, rcvCount[procRank], MPI_DOUBLE, resMat, rcvCount, rcvIndices, MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

	for (i = 0; i < procCount; ++i) {
		rcvCount[i] /= n;
		rcvIndices[i] /= n;
	}

	MPI_Gatherv(procVec, rcvCount[procRank], MPI_DOUBLE, resVec, rcvCount, rcvIndices, MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

	free(rcvCount);
	free(rcvIndices);
}

void setArraysValues(int **count, int **indices, const int n) {
	int rowsCount, rowsRest, i;

	*count = (int *)malloc(procCount * sizeof(int));
	*indices = (int *)malloc(procCount * sizeof(int));

	rowsRest = n;
	rowsCount = n / procCount;
	(*count)[0] = rowsCount * n;
	(*indices)[0] = 0;
	for (i = 1; i < procCount; ++i) {
		rowsRest -= rowsCount;
		rowsCount = rowsRest / (procCount - i);
		(*count)[i] = rowsCount * n;
		(*indices)[i] = (*indices)[i - 1] + (*count)[i - 1];
	}

	/*То, что выше, лучше
	sendCount[0] = (n / procCount + n % procCount) * n;
	sendIndices[0] = 0;
	for (i = 1; i < procCount; ++i) {
		sendCount[i] = n / procCount * n;
		sendIndices[i] = sendIndices[i - 1] + sendCount[i - 1];
	}
	*/
}

void swapRows(double *r1, double *r2, const int n) {
	int i;

	for (i = 0; i < n; ++i)
		swap(&r1[i], &r2[i]);
}

void swap(double *v1, double *v2) {
	double tmp = *v1;
	*v1 = *v2;
	*v2 = tmp;
}